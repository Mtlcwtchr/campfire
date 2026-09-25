#include "engine/geometry/smart_terrain_mesh.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace engine {
namespace {
struct Point { int x, y; };
struct Triangle { Point a, b, c; };
Point midpoint(Triangle t) { return {(t.a.x + t.b.x) / 2, (t.a.y + t.b.y) / 2}; }
int area(Triangle t) {
    return (t.b.x-t.a.x)*(t.c.y-t.a.y) - (t.b.y-t.a.y)*(t.c.x-t.a.x);
}
std::array<Triangle, 2> children(Triangle t) {
    const auto m = midpoint(t);
    return {Triangle{t.c, t.a, m}, Triangle{t.b, t.c, m}};
}
std::array<Triangle, 2> roots(int n) {
    return {Triangle{{n,0},{0,n},{0,0}}, Triangle{{0,n},{n,0},{n,n}}};
}
std::array<double, 3> weights(Triangle t, double x, double y) {
    const double det = area(t);
    const double b = ((x-t.a.x)*(t.c.y-t.a.y) - (y-t.a.y)*(t.c.x-t.a.x)) / det;
    const double c = ((t.b.x-t.a.x)*(y-t.a.y) - (t.b.y-t.a.y)*(x-t.a.x)) / det;
    return {1-b-c,b,c};
}
}

std::array<float, 2> SmartTerrainMesh::sample(double x, double y) const {
    x = std::clamp(x / step, 0.0, double(cells));
    y = std::clamp(y / step, 0.0, double(cells));
    auto t = roots(cells)[x+y <= cells ? 0 : 1];
    const auto at = [&](Point p) { return std::size_t(p.y) * (cells+1) + p.x; };
    while (area(t) > 1 && splits[at(midpoint(t))]) {
        const auto next = children(t);
        const auto w = weights(next[0], x, y);
        t = *std::min_element(w.begin(), w.end()) >= -1e-9 ? next[0] : next[1];
    }
    const auto w = weights(t, x, y);
    const auto a = at(t.a), b = at(t.b), c = at(t.c);
    return {float(bed[a]*w[0] + bed[b]*w[1] + bed[c]*w[2]),
            float(head[a]*w[0] + head[b]*w[1] + head[c]*w[2])};
}
std::size_t SmartTerrainMesh::bytes() const {
    return (bed.size()+head.size()+prior.size())*sizeof(float) + splits.size() +
        vertices.size()*sizeof(AdaptiveVertex) + indices.size()*sizeof(std::uint32_t);
}

float SmartTerrainMesh::samplePrior(double x,double y) const {
    if (prior.empty()) return sample(x,y)[0];
    x=std::clamp(x/step,0.0,double(cells)); y=std::clamp(y/step,0.0,double(cells));
    auto t=roots(cells)[x+y<=cells?0:1];
    const auto at=[&](Point p){return std::size_t(p.y)*(cells+1)+p.x;};
    while (area(t)>1 && splits[at(midpoint(t))]) {
        const auto next=children(t); const auto w=weights(next[0],x,y);
        t=*std::min_element(w.begin(),w.end())>=-1e-9?next[0]:next[1];
    }
    const auto w=weights(t,x,y);
    return float(prior[at(t.a)]*w[0]+prior[at(t.b)]*w[1]+prior[at(t.c)]*w[2]);
}

std::shared_ptr<const SmartTerrainMesh> makeAdaptiveMesh(int n, double step, double tolerance,
    const SurfaceSample& surface, const SurfaceSample& parent, double morphTolerance,
    const SurfaceTolerance& localTolerance, const HeightSample& prior, const HeightSample& priorParent,
    AdaptiveCriteria criteria) {
    if (n < 1 || n > 256 || (n & (n-1)) || !(step > 0) || !std::isfinite(step) ||
        !(tolerance >= 0) || !std::isfinite(tolerance) ||
        !(morphTolerance >= 0) || !std::isfinite(morphTolerance) || !surface ||
        criteria.boundaryStride<1 || criteria.boundaryStride>n ||
        (criteria.boundaryStride&(criteria.boundaryStride-1)) ||
        !(criteria.normalErrorDegrees>=0 && criteria.normalErrorDegrees<=90) ||
        !(criteria.normalHeightFloor>=0) || !std::isfinite(criteria.normalHeightFloor))
        throw std::invalid_argument("invalid adaptive terrain lattice");
    auto out = std::make_shared<SmartTerrainMesh>();
    out->cells = n; out->step = step; out->tolerance = tolerance;
    const auto count = std::size_t(n+1)*(n+1);
    const auto at = [&](Point p) { return std::size_t(p.y)*(n+1)+p.x; };
    out->bed.resize(count); out->head.resize(count); out->prior.resize(count); out->splits.resize(count);
    std::vector<double> limits(count,tolerance);
    std::vector<float> parentBed(count), parentHead(count), parentPrior(count);
    for (int y = 0; y <= n; ++y) for (int x = 0; x <= n; ++x) {
        const auto i = at({x,y});
        const auto s = surface(x*step,y*step);
        const auto p = parent ? parent(x*step,y*step) : s;
        out->prior[i]=prior?prior(x*step,y*step):s[0];
        parentPrior[i]=priorParent?priorParent(x*step,y*step):out->prior[i];
        if (localTolerance) {
            limits[i]=localTolerance(x*step,y*step);
            if (!(limits[i]>=0) || !std::isfinite(limits[i]))
                throw std::invalid_argument("invalid local terrain tolerance");
        }
        for (float v : {s[0],s[1],p[0],p[1],out->prior[i],parentPrior[i]})
            if (!std::isfinite(v)) throw std::invalid_argument("nonfinite adaptive terrain height");
        out->bed[i]=s[0]; out->head[i]=s[1]; parentBed[i]=p[0]; parentHead[i]=p[1];
    }
    // Breadth-first enumeration is important: both halves of every diamond
    // must finish before propagating their shared errors to the next depth.
    const auto root = roots(n);
    std::vector<Triangle> tree{root[0],root[1]};
    tree.reserve(std::size_t(n)*n*4);
    for (std::size_t i = 0; i < tree.size(); ++i) if (area(tree[i]) > 1) {
        const auto next = children(tree[i]);
        tree.insert(tree.end(), next.begin(), next.end());
    }
    std::vector<double> errors(count), morphErrors(count);
    const double normalCos=std::cos(criteria.normalErrorDegrees*std::acos(-1.0)/180.0);
    const auto normal=[&](Triangle t,const auto& values) {
        const double a=values[at(t.a)],b=values[at(t.b)],c=values[at(t.c)];
        const double det=area(t)*step;
        const double gx=((b-a)*(t.c.y-t.a.y)-(c-a)*(t.b.y-t.a.y))/det;
        const double gy=((c-a)*(t.b.x-t.a.x)-(b-a)*(t.c.x-t.a.x))/det;
        const double length=std::sqrt(1+gx*gx+gy*gy);
        return std::array{-gx/length,-gy/length,1/length};
    };
    for (auto it = tree.rbegin(); it != tree.rend(); ++it) {
        const auto t = *it;
        if (area(t) <= 1) continue;
        const auto m = midpoint(t);
        const auto a = at(t.a), b = at(t.b), mid = at(m);
        limits[mid]=std::min({limits[mid],limits[a],limits[b],limits[at(t.c)]});
        const auto deviation = [&](const auto& values) {
            return std::abs(double(values[mid]) - (double(values[a])+values[b])*0.5);
        };
        const double raw = std::max({deviation(out->bed), deviation(out->head),deviation(out->prior)});
        if (area(t) <= 4) out->detailError = std::max({out->detailError,deviation(out->bed),deviation(out->prior)});
        double error = raw, morph = parent || priorParent ? std::max({deviation(parentBed),deviation(parentHead),deviation(parentPrior)}) : 0;
        const auto next = children(t);
        // Height alone can discard a low but sharp ridge or tributary. Compare
        // directions, not absolute slope: even a steep plane costs no interior
        // subdivisions. Ignore sub-decimetre/quantisation noise explicitly.
        const auto bends=[&](const auto& values) {
            if (criteria.normalErrorDegrees==0 || deviation(values)<=criteria.normalHeightFloor) return false;
            const auto coarse=normal(t,values);
            for (const auto child:next) {
                const auto fine=normal(child,values);
                if (coarse[0]*fine[0]+coarse[1]*fine[1]+coarse[2]*fine[2]<normalCos) return true;
            }
            return false;
        };
        if (bends(out->bed) || bends(out->prior)) error=std::numeric_limits<double>::infinity();
        // A 25 cm parent-height allowance can still erase a sharp small crest
        // at the beginning of a morph. Protect the normals of both LOD parent
        // endpoints as well as the two source stages; water remains height-only.
        if ((parent && bends(parentBed)) || (priorParent && bends(parentPrior)))
            morph=std::numeric_limits<double>::infinity();
        if (area(next[0]) > 1) {
            const auto left = at(midpoint(next[0])), right = at(midpoint(next[1]));
            error += std::max(errors[left],errors[right]);
            morph += std::max(morphErrors[left],morphErrors[right]);
            limits[mid]=std::min({limits[mid],limits[left],limits[right]});
        }
        // Keep the nominal edge lattice; finer boundary points are error-driven
        // and are stitched against the actual neighbouring edge segments.
        if (((m.x==0 || m.x==n) && m.y%criteria.boundaryStride==0) ||
            ((m.y==0 || m.y==n) && m.x%criteria.boundaryStride==0))
            error = std::numeric_limits<double>::infinity();
        errors[mid] = std::max(errors[mid],error);
        morphErrors[mid] = std::max(morphErrors[mid],morph);
    }
    for (std::size_t i = 0; i < count; ++i)
        out->splits[i] = errors[i] > limits[i] || morphErrors[i] > morphTolerance;
    std::vector<std::array<std::int32_t,6>> vertexIds(count,{-1,-1,-1,-1,-1,-1});
    const auto emit = [&](Point a, Point b, Point c, int sa=0, int sb=0, int sc=0) {
        const Point points[]{a,b,c}; const int skirts[]{sa,sb,sc};
        for (int corner = 0; corner < 3; ++corner) {
            const auto p = points[corner];
            const auto i = at(p);
            auto& id=vertexIds[i][skirts[corner]*3+corner];
            if (id<0) {
                id=static_cast<std::int32_t>(out->vertices.size());
                out->vertices.push_back({std::uint16_t(p.x),std::uint16_t(p.y),
                    std::uint16_t(skirts[corner]|4),std::uint16_t(corner),parentBed[i],parentHead[i],
                    0,0,out->bed[i],out->head[i],out->prior[i],parentPrior[i],0,{}});
            }
            out->indices.push_back(static_cast<std::uint32_t>(id));
        }
    };
    std::vector<std::array<Point,2>> boundary;
    const auto onEdge=[&](Point a,Point b) {
        return (a.x==b.x && (a.x==0 || a.x==n)) || (a.y==b.y && (a.y==0 || a.y==n));
    };
    const auto visit = [&](auto&& self, Triangle t) -> void {
        if (area(t)>1 && out->splits[at(midpoint(t))]) {
            for (const auto child : children(t)) self(self,child);
        } else {
            emit(t.a,t.b,t.c);
            const Point p[]{t.a,t.b,t.c,t.a};
            for (int e=0;e<3;++e) if (onEdge(p[e],p[e+1])) boundary.push_back({p[e],p[e+1]});
        }
    };
    for (const auto t : root) visit(visit,t);
    out->surfaceIndices = static_cast<std::uint32_t>(out->indices.size());

    // The wet triangles first, and how many there are.
    //
    // The water pass used to draw the WHOLE surface of any square that held any
    // water at all, and let the pixel shader throw away what turned out to be
    // dry. A square with a stream across a corner of it therefore rasterised
    // the water shader - waves, ice, climate, weather, two screen-space
    // derivatives and a pair of noise lookups - over every pixel of its ground,
    // and discarded ninety-odd per cent of that at the very end. Two expensive
    // shaders over every pixel of the landscape is what a frame was being spent
    // on, and it is why taking triangles away did not move it.
    //
    // A triangle is worth drawing water on when any of its corners has water
    // standing over it. Partitioned in place rather than copied: the wet ones
    // move to the front, the count is recorded, and the dry ones stay where
    // they are for the terrain pass, which still wants all of them.
    {
        // Wet, or close enough to be part of the shore.
        //
        // The water pass deliberately draws onto dry land: the swash, the foam
        // and the wet band above the waterline are all fragments whose depth is
        // NEGATIVE, and the shader keeps the sign precisely so it can shade
        // them. Cutting the prefix at depth zero therefore took the shoreline
        // away along with the overdraw - the water ended in a hard line with no
        // run-up, which is the whole of what a shore looks like.
        //
        // Three metres of apron. That is the width of a swash, it costs a ring
        // of triangles round each body rather than the whole square, and the
        // saving this was for is untouched: the ground away from water is still
        // not drawn twice.
        constexpr float kApronMetres = 3.0f;
        const auto wet = [&](std::uint32_t index) {
            const auto& v = out->vertices[index];
            return v.sourceHead > v.sourceBed - kApronMetres;
        };
        std::uint32_t front = 0;
        for (std::uint32_t i = 0; i + 2 < out->surfaceIndices; i += 3) {
            if (!(wet(out->indices[i]) || wet(out->indices[i + 1]) || wet(out->indices[i + 2])))
                continue;
            if (i != front)
                for (int k = 0; k < 3; ++k) std::swap(out->indices[front + k], out->indices[i + k]);
            front += 3;
        }
        out->wetIndices = front;
    }
    // Skirts follow real surface edges, not invisible discarded sample points.
    for (const auto& edge:boundary) {
        const auto a=edge[0],b=edge[1];
        emit(a,a,b,0,1,0); emit(b,a,b,0,1,1);
    }
    // Discard probe rows that contributed no rendered vertices. Flat chunks
    // recover their nominal large step and small cache footprint; a neighbouring
    // ridge can retain the fine lattice without forcing this chunk to do so.
    int stride=1;
    while (stride<criteria.boundaryStride && std::all_of(out->vertices.begin(),out->vertices.end(),
        [&](const auto& v){return v.x%(stride*2)==0 && v.y%(stride*2)==0;})) stride*=2;
    if (stride>1) {
        const int cells=n/stride;
        const auto compact=[&](auto& values) {
            auto smaller=values;
            smaller.resize(std::size_t(cells+1)*(cells+1));
            for (int y=0;y<=cells;++y) for (int x=0;x<=cells;++x)
                smaller[std::size_t(y)*(cells+1)+x]=values[at({x*stride,y*stride})];
            smaller.shrink_to_fit();
            values=std::move(smaller);
        };
        compact(out->bed);compact(out->head);compact(out->prior);compact(out->splits);
        for (auto& v:out->vertices) { v.x/=stride;v.y/=stride; }
        out->cells=cells;out->step=step*stride;
    }
    return out;
}
} // namespace engine


