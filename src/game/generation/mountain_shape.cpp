#include "game/generation/mountain_shape.hpp"
#include "engine/core/rng.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <numeric>
#include <stdexcept>

namespace generation::mountains {
namespace {
constexpr int kGridStep=512,kHalfExtent=kSupportMetres,kGridSide=2*kHalfExtent/kGridStep;
static_assert(2*kHalfExtent%kGridStep==0);
static_assert(kHalfExtent<3*kRegionMetres/2); // the 3x3 regional stencil still covers all support
constexpr int kGeneration=1;
struct Point { Fixed x,y; };

Fixed fraction(std::uint64_t key) { return Fixed::ratio(static_cast<std::int64_t>(key&65535),65535); }
std::uint64_t regionKey(std::uint64_t seed,std::int64_t x,std::int64_t y) {
    return core::splitmix64(seed^core::splitmix64(static_cast<std::uint64_t>(x))^
                           (core::splitmix64(static_cast<std::uint64_t>(y))<<1)^kGeneration);
}

class Builder {
public:
    Skeleton result;
    std::uint16_t node(Point p,Fixed height,Fixed width) {
        const auto id=result.nodeCount++;
        result.nodes[id]={p.x,p.y,height,width,core::kZero,0};
        return id;
    }
    void edge(std::uint16_t from,std::uint16_t to,int order) {
        result.edges[result.edgeCount++]={from,to,static_cast<std::uint8_t>(order)};
        ++result.nodes[from].degree; ++result.nodes[to].degree;
    }
    void branch(std::uint16_t from,Point end,Fixed width,int depth,std::uint64_t key) {
        // Two daughters per branch, not per junction: five complete orders
        // stay bounded at 279 edges / 280 nodes without capacity truncation.
        if (depth>=kBranchGenerations || result.nodeCount+kSegmentsPerBranch>kMaxNodes ||
            result.edgeCount+kSegmentsPerBranch>kMaxEdges) return;
        const auto start=result.nodes[from]; // copied: child insertion must not change it
        const auto dx=end.x-start.x,dy=end.y-start.y;
        const auto length=core::hypot(dx,dy);
        if (length<Fixed::fromInt(180)) return;
        const auto tangent=Point{dx/length,dy/length};
        std::array<std::uint16_t,2> junctions{};
        auto previous=from;
        for (std::size_t i=0;i<junctions.size();++i) {
            const auto h=core::splitmix64(key+i*0x6a09+0xe667);
            const auto t=Fixed::ratio(static_cast<std::int64_t>(i)+1,3)+
                (fraction(h)-Fixed::ratio(1,2))*Fixed::ratio(1,20);
            const auto bend=(fraction(core::splitmix64(h))-Fixed::ratio(1,2))*length*Fixed::ratio(1,5);
            const Point p{start.x+dx*t-tangent.y*bend,start.y+dy*t+tangent.x*bend};
            const auto retained=i==0 ? Fixed::ratio(93,100)+fraction(h)*Fixed::ratio(5,100) :
                                      Fixed::ratio(78,100)+fraction(h)*Fixed::ratio(8,100);
            junctions[i]=node(p,start.height*retained,width);
            edge(previous,junctions[i],depth);
            previous=junctions[i];
        }
        const auto tip=node(end,core::kZero,width*Fixed::ratio(3,4));
        edge(previous,tip,depth);
        if (depth+1==kBranchGenerations) return;
        // Stagger opposite-side daughters along the spine. Swapping sides by
        // seed avoids a repeated comb, while shared nodes keep every join exact.
        for (std::size_t i=0;i<junctions.size();++i) {
            const int sign=((key>>32)&1)==i ? -1 : 1;
            const auto child=core::splitmix64(key^static_cast<std::uint64_t>(sign)^0xb7a9);
            const auto reach=length*(Fixed::ratio(3,5)+fraction(child)*Fixed::ratio(1,10));
            // Oblique, forward-growing spurs spread the outer skeleton rather
            // than curling almost perpendicular daughters back into the core.
            const auto forward=Fixed::ratio(9+static_cast<int>((child>>18)%5),10);
            Point direction{tangent.x*forward-tangent.y*Fixed::fromInt(sign),
                            tangent.y*forward+tangent.x*Fixed::fromInt(sign)};
            const auto norm=core::hypot(direction.x,direction.y);
            direction={direction.x/norm,direction.y/norm};
            const auto junction=result.nodes[junctions[i]];
            branch(junctions[i],{junction.x+direction.x*reach,junction.y+direction.y*reach},
                   width*Fixed::ratio(31,50),depth+1,child);
        }
    }
};

struct Segment {
    Fixed x,y,dx,dy,unitX,unitY,inverseLength;
    Fixed fromHeight,toHeight,fromWidth,toWidth;
};
struct Cell {
    std::array<std::uint64_t,(kMaxEdges+63)/64> edges{};
    std::array<std::uint64_t,(kMaxNodes+63)/64> nodes{};
};
struct Cached {
    bool valid=false;
    std::uint64_t seed=0,age=0;
    std::int64_t rx=0,ry=0;
    bool regional=false;
    Skeleton skeleton;
    std::array<Segment,kMaxEdges> segments{};
    std::array<Cell,kGridSide*kGridSide> cells{};
};

Segment segmentOf(const Skeleton& s,const Edge& e) {
    const auto& a=s.nodes[e.from]; const auto& b=s.nodes[e.to];
    const auto dx=b.x-a.x,dy=b.y-a.y;
    const auto length=core::hypot(dx,dy);
    return {a.x,a.y,dx,dy,dx/length,dy/length,core::kOne/length,a.height,b.height,a.width,b.width};
}
Fixed segmentHeight(const Segment& s,Fixed x,Fixed y) {
    // Reciprocal length, not reciprocal squared length: the latter loses too
    // many Fixed bits on kilometre-long edges and shifts their endpoints.
    const auto t=core::saturate(((x-s.x)*s.unitX+(y-s.y)*s.unitY)*s.inverseLength);
    const auto dx=x-(s.x+s.dx*t),dy=y-(s.y+s.dy*t);
    const auto width=core::lerp(s.fromWidth,s.toWidth,t);
    // Cheap reject before the square root, by distance to the centreline.
    if (core::abs(dx)>=width || core::abs(dy)>=width) return {};
    // Keep the inner spine elevated; only terminal segments ease down to a
    // zero-height, zero-slope ending instead of halving at every junction.
    const auto height=core::lerp(s.fromHeight,s.toHeight,s.toHeight==core::kZero ? smooth(t) : t);
    const auto saddle=core::kOne-t*(core::kOne-t)*Fixed::ratio(2,5);
    return crest(core::hypot(dx,dy),width)*height*saddle;
}
Fixed nodeLift(const Node& n,Fixed x,Fixed y) {
    if (n.lift<=core::kZero) return {};
    const auto radius=n.width/2;
    const auto dx=x-n.x,dy=y-n.y;
    if (core::abs(dx)>=radius || core::abs(dy)>=radius) return {};
    return n.lift*crest(core::hypot(dx,dy),radius);
}
void index(Cached& cache) {
    cache.cells={};
    const auto cover=[&](Fixed left,Fixed top,Fixed right,Fixed bottom,std::size_t id,bool node) {
        const auto cell=[](Fixed p) { return static_cast<int>(floorDiv(p.toInt()+kHalfExtent,kGridStep)); };
        const int x0=std::max(0,cell(left)),y0=std::max(0,cell(top));
        const int x1=std::min(kGridSide-1,cell(right)),y1=std::min(kGridSide-1,cell(bottom));
        for (int y=y0;y<=y1;++y) for (int x=x0;x<=x1;++x) {
            auto& c=cache.cells[y*kGridSide+x];
            const auto bit=std::uint64_t{1}<<(id%64);
            if (node) c.nodes[id/64]|=bit;
            else c.edges[id/64]|=bit;
        }
    };
    const auto& s=cache.skeleton;
    for (std::size_t i=0;i<s.edgeCount;++i) {
        const auto& e=s.edges[i]; const auto& a=s.nodes[e.from]; const auto& b=s.nodes[e.to];
        cache.segments[i]=segmentOf(s,e);
        const auto width=std::max(a.width,b.width);
        cover(std::min(a.x,b.x)-width,std::min(a.y,b.y)-width,
              std::max(a.x,b.x)+width,std::max(a.y,b.y)+width,i,false);
    }
    for (std::size_t i=0;i<s.nodeCount;++i) {
        const auto& n=s.nodes[i]; if (n.lift<=core::kZero) continue;
        cover(n.x-n.width/2,n.y-n.width/2,n.x+n.width/2,n.y+n.width/2,i,true);
    }
}
Cached& cached(std::uint64_t seed,std::int64_t rx,std::int64_t ry,bool regional) {
    // Private to a worker, fixed storage, no allocation/lock on height queries.
    // LRU rather than direct mapping: nine neighbouring regions must coexist.
    struct Cache { std::array<Cached,16> entries{}; std::uint64_t clock=0; };
    static thread_local Cache cache;
    Cached* victim=&cache.entries.front();
    for (auto& entry : cache.entries) {
        if (entry.valid && entry.seed==seed && entry.rx==rx && entry.ry==ry && entry.regional==regional) {
            entry.age=++cache.clock; return entry;
        }
        if (!entry.valid || (victim->valid && entry.age<victim->age)) victim=&entry;
    }
    victim->seed=seed; victim->rx=rx; victim->ry=ry; victim->regional=regional;
    victim->skeleton=regional ? regionSkeleton(seed,rx,ry) : makeSkeleton(seed);
    index(*victim); victim->age=++cache.clock; victim->valid=true;
    return *victim;
}
Shape sampleIndexed(const Cached& cache,Fixed x,Fixed y) {
    const auto cx=floorDiv(x.toInt()+kHalfExtent,kGridStep),cy=floorDiv(y.toInt()+kHalfExtent,kGridStep);
    if (cx<0 || cy<0 || cx>=kGridSide || cy>=kGridSide) return {};
    const auto& cell=cache.cells[static_cast<std::size_t>(cy)*kGridSide+cx];
    Shape out;
    for (std::size_t word=0;word<cell.edges.size();++word) {
        for (auto bits=cell.edges[word];bits;bits&=bits-1) {
            const auto id=word*64+static_cast<unsigned>(std::countr_zero(bits));
            out.body=std::max(out.body,segmentHeight(cache.segments[id],x,y));
        }
    }
    for (std::size_t word=0;word<cell.nodes.size();++word) {
        for (auto bits=cell.nodes[word];bits;bits&=bits-1) {
            const auto id=word*64+static_cast<unsigned>(std::countr_zero(bits));
            out.junction=std::max(out.junction,nodeLift(cache.skeleton.nodes[id],x,y));
        }
    }
    return out;
}
} // namespace

Skeleton makeSkeleton(std::uint64_t seed) {
    Builder build;
    const auto key=core::splitmix64(seed^0xf7ac7a1);
    const auto root=build.node({core::kZero,core::kZero},
        Fixed::ratio(78+static_cast<int>(key%21),100),Fixed::fromInt(1100));
    // Three nonparallel primary arms. Regional rotation below varies from one
    // massif to the next instead of selecting one direction for the whole map.
    constexpr int directions[3][2]={{5,1},{-2,5},{-4,-4}};
    for (int i=0;i<3;++i) {
        const auto h=core::splitmix64(key+i*731);
        Point d{Fixed::fromInt(directions[i][0])+fraction(h)-Fixed::ratio(1,2),
                Fixed::fromInt(directions[i][1])+fraction(core::splitmix64(h))-Fixed::ratio(1,2)};
        const auto norm=core::hypot(d.x,d.y);
        const auto length=Fixed::fromInt(3000+static_cast<int>((h>>20)%1200));
        build.branch(root,{d.x/norm*length,d.y/norm*length},
                     Fixed::fromInt(750+static_cast<int>((h>>34)%201)),0,h);
    }
    auto& result=build.result;
    for (std::size_t i=0;i<result.nodeCount;++i) {
        auto& n=result.nodes[i];
        if (n.degree<3) continue;
        const auto h=core::splitmix64(key+i*127);
        n.lift=n.height*Fixed::ratio(5+static_cast<int>(h%10),100);
    }
    return result;
}

Skeleton regionSkeleton(std::uint64_t seed,std::int64_t rx,std::int64_t ry) {
    const auto key=regionKey(seed,rx,ry);
    auto s=makeSkeleton(key);
    constexpr int direction[8][2]={{5,0},{4,3},{0,5},{-3,4},{-5,0},{-4,-3},{0,-5},{3,-4}};
    const auto& d=direction[key%8];
    const auto a=Fixed::ratio(d[0],5),b=Fixed::ratio(d[1],5);
    const auto jitterX=Fixed::fromInt(static_cast<int>((key>>8)%2201)-1100);
    const auto jitterY=Fixed::fromInt(static_cast<int>((key>>24)%2201)-1100);
    for (std::size_t i=0;i<s.nodeCount;++i) {
        auto& n=s.nodes[i]; const auto x=n.x,y=n.y;
        n.x=a*x-b*y+jitterX; n.y=b*x+a*y+jitterY;
    }
    s.centreX=jitterX; s.centreY=jitterY;
    return s;
}

Shape sampleSkeleton(const Skeleton& skeleton,Fixed x,Fixed y) {
    Shape out;
    for (std::size_t i=0;i<skeleton.edgeCount;++i)
        out.body=std::max(out.body,segmentHeight(segmentOf(skeleton,skeleton.edges[i]),x,y));
    for (std::size_t i=0;i<skeleton.nodeCount;++i)
        out.junction=std::max(out.junction,nodeLift(skeleton.nodes[i],x,y));
    return out;
}

Shape sample(std::uint64_t seed,Fixed x,Fixed y) {
    return sampleIndexed(cached(seed,0,0,false),x,y);
}

Shape worldSample(std::uint64_t seed,Fixed x,Fixed y) {
    const auto rx=floorDiv(x.raw,Fixed::fromInt(kRegionMetres).raw);
    const auto ry=floorDiv(y.raw,Fixed::fromInt(kRegionMetres).raw);
    Shape out;
    // Neighbours, not chunk-local clipping. All five orders, including
    // rotation, widths and jitter, fit inside 11.264 km of the nominal centre,
    // less than the nearest omitted region (1.5 * 8.192 km away).
    for (auto gy=ry-1;gy<=ry+1;++gy) for (auto gx=rx-1;gx<=rx+1;++gx) {
        const auto px=x-Fixed::fromInt(gx*kRegionMetres+kRegionMetres/2);
        const auto py=y-Fixed::fromInt(gy*kRegionMetres+kRegionMetres/2);
        if (core::abs(px)>=Fixed::fromInt(kHalfExtent) || core::abs(py)>=Fixed::fromInt(kHalfExtent)) continue;
        const auto shape=sampleIndexed(cached(seed,gx,gy,true),px,py);
        // A union, not addition of every overlap: keep heights bounded and
        // avoid duplicate hills at region borders. Actual graph nodes add lift.
        if (shape.height()>out.height()) out=shape;
    }
    return out;
}
std::vector<std::uint16_t> drainageIncisions(const std::vector<Fixed>& heights,
        const std::vector<Fixed>& limits,int width,int height,int spacing) {
    if (width<2 || height<2 || spacing<=0 || heights.size()!=std::size_t(width)*height ||
        limits.size()!=heights.size()) throw std::invalid_argument("invalid incision grid");
    const auto count=heights.size();
    std::vector<std::size_t> order(count),down(count);
    std::iota(order.begin(),order.end(),0);
    std::iota(down.begin(),down.end(),0);
    std::sort(order.begin(),order.end(),[&](auto a,auto b) {
        return heights[a]!=heights[b] ? heights[a]>heights[b] : a<b;
    });
    std::vector<std::uint32_t> flow(count,1);
    std::vector<Fixed> slope(count),depth(count);
    for (int y=1;y<height-1;++y) for (int x=1;x<width-1;++x) {
        const auto i=std::size_t(y)*width+x;
        for (int dy=-1;dy<=1;++dy) for (int dx=-1;dx<=1;++dx) {
            if (!dx && !dy) continue;
            const auto j=std::size_t(y+dy)*width+x+dx;
            const auto run=Fixed::fromInt(spacing)*(dx && dy ? Fixed::ratio(1414,1000) : core::kOne);
            const auto s=(heights[i]-heights[j])/run;
            if (s>slope[i]) { slope[i]=s; down[i]=j; }
        }
    }
    for (auto i : order) if (down[i]!=i) flow[down[i]]+=flow[i];
    for (auto i : order) {
        // Headwaters start gently; tributaries merge into stronger incisions.
        // Broad slopes erode too, but neither flat plateaus nor summits become
        // a stippled field of holes. No invented drainage across existing pits.
        const auto catchment=core::saturate((core::sqrt(Fixed::fromInt(flow[i]))-core::kOne)/8);
        const auto grade=smooth((slope[i]-Fixed::ratio(1,100))/Fixed::ratio(18,100));
        depth[i]=std::max(core::kZero,limits[i])*catchment*grade;
    }
    // Spread a channel into its banks rather than cutting grid-width slots.
    auto softened=depth;
    for (int y=1;y<height-1;++y) for (int x=1;x<width-1;++x) {
        const auto i=std::size_t(y)*width+x;
        Fixed sum=depth[i]*8;
        for (int dy=-1;dy<=1;++dy) for (int dx=-1;dx<=1;++dx)
            if (dx || dy) sum+=depth[std::size_t(y+dy)*width+x+dx];
        softened[i]=std::min(std::max(core::kZero,limits[i]),sum/16);
    }
    // Downstream first: incision must not turn a previously descending edge
    // uphill. The public result is quantised here, so compare quantised depths.
    std::vector<std::uint16_t> result(count);
    for (auto it=order.rbegin();it!=order.rend();++it) {
        const auto i=*it,j=down[i];
        auto dm=std::clamp<std::int64_t>((softened[i]*10).toInt(),0,3000);
        if (j==i) dm=0;
        else dm=std::min(dm,(std::max(core::kZero,heights[i]-heights[j])*10).toInt()+result[j]);
        result[i]=static_cast<std::uint16_t>(dm);
    }
    return result;
}
} // namespace generation::mountains
