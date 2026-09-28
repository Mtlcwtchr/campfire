#include "engine/render/impostor_hierarchy.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>
#include <set>
#include <stdexcept>

namespace engine::render {
ImpostorKey hierarchyKey(int level,double x,double y) {
    if (level<0 || level>=kHierarchyLevels) throw std::invalid_argument("invalid impostor hierarchy level");
    const auto cell=double(hierarchyCell(level));
    return {std::int64_t(std::floor(x/cell))*hierarchyCell(level),std::int64_t(std::floor(y/cell))*hierarchyCell(level),level};
}
ImpostorKey hierarchyParent(ImpostorKey key) {
    return hierarchyKey(key.level+1,double(key.x),double(key.y));
}
std::array<ImpostorKey,16> hierarchyChildren(ImpostorKey key) {
    if (key.level<=0) throw std::invalid_argument("hierarchy leaf has no children");
    std::array<ImpostorKey,16> children{};
    const auto step=hierarchyCell(key.level-1);
    for (int j=0;j<4;++j) for (int i=0;i<4;++i) children[j*4+i]={key.x+i*step,key.y+j*step,key.level-1};
    return children;
}
bool hierarchyContains(ImpostorKey ancestor,ImpostorKey key) {
    if (key.level>ancestor.level) return false;
    const auto cell=hierarchyCell(ancestor.level);
    return key.x>=ancestor.x && key.x<ancestor.x+cell && key.y>=ancestor.y && key.y<ancestor.y+cell;
}

std::shared_ptr<const ImpostorAtlas> resampleImpostor(const ImpostorAtlas& source,unsigned resolution) {
    if (!source.valid() || resolution<1 || resolution>source.resolution || source.resolution%resolution ||
        (resolution&(resolution-1))) return {};
    const unsigned factor=source.resolution/resolution;
    auto out=std::make_shared<ImpostorAtlas>();
    out->resolution=resolution;out->centre=source.centre;out->side=source.side;
    out->members=source.members;out->levels=source.levels;
    // Coarser texels move a surface by up to a whole output texel.
    out->error=source.error+2*source.side/resolution;
    const auto pixels=std::size_t(resolution)*resolution,sourcePixels=std::size_t(source.resolution)*source.resolution;
    out->colour.resize(pixels*kHemisphereViews);out->normal.resize(out->colour.size());out->depth.resize(out->colour.size());
    for (unsigned view=0;view<kHemisphereViews;++view) for (unsigned y=0;y<resolution;++y) for (unsigned x=0;x<resolution;++x) {
        double alpha=0,rgb[3]{},normal[3]{};int nearest=-1;unsigned nearestCode=0;
        for (unsigned j=0;j<factor;++j) for (unsigned i=0;i<factor;++i) {
            const auto at=view*sourcePixels+std::size_t(y*factor+j)*source.resolution+x*factor+i;
            const double a=source.colour[at][3];if (!(a>0)) continue;
            alpha+=a;
            for (int k=0;k<3;++k) {rgb[k]+=source.colour[at][k]*a;normal[k]+=(source.normal[at][k]/127.5-1)*a;}
            const unsigned code=(unsigned(source.depth[at][0])<<8)|source.depth[at][1];
            if (nearest<0 || code>nearestCode) {nearest=int(at);nearestCode=code;}
        }
        const auto target=view*pixels+std::size_t(y)*resolution+x;
        const double coverage=alpha/(factor*factor);
        const auto length=std::sqrt(normal[0]*normal[0]+normal[1]*normal[1]+normal[2]*normal[2]);
        for (int k=0;k<3;++k) {
            out->colour[target][k]=std::uint8_t(std::clamp(std::lround(alpha>0?rgb[k]/alpha:0),0l,255l));
            const double unit=length>1e-9?normal[k]/length:(k==2?1.0:0.0);
            out->normal[target][k]=std::uint8_t(std::clamp(std::lround((unit*.5+.5)*255),0l,255l));
        }
        const auto a=std::uint8_t(std::clamp(std::lround(coverage),0l,255l));
        out->colour[target][3]=out->normal[target][3]=a;
        if (nearest>=0) out->depth[target]={source.depth[std::size_t(nearest)][0],source.depth[std::size_t(nearest)][1],std::uint8_t(view),a};
        else out->depth[target]={128,0,std::uint8_t(view),0};
    }
    buildImpostorMips(*out);
    return out;
}

namespace {
ImpostorBasis basisFor(double angle,double elevation) {
    const double c=std::cos(angle),s=std::sin(angle),ce=std::cos(elevation),se=std::sin(elevation);
    return {{c,s,0},{s*se,-c*se,ce},{-s*ce,c*ce,se}};
}
// Every covered texel of one view as a point relative to the atlas centre.
// `stride` skips texels finer than the grid they are splatted into: a 128²
// leaf seen in a 64² parent has four points per output texel, three wasted.
std::vector<camera::Vec3> surface(const ImpostorAtlas& atlas,unsigned view,unsigned stride=1) {
    std::vector<camera::Vec3> points;
    const unsigned n=atlas.resolution;const auto pixels=std::size_t(n)*n;const auto basis=*hemisphereBasis(view);
    stride=std::max(1u,stride);
    for (unsigned y=stride/2;y<n;y+=stride) for (unsigned x=stride/2;x<n;x+=stride) {
        const auto at=view*pixels+std::size_t(y)*n+x;
        if (atlas.colour[at][3]<51 || atlas.depth[at][3]<51) continue;
        const double u=((x+.5)/n-.5)*atlas.side,w=(.5-(y+.5)/n)*atlas.side;
        const double d=impostorDepth(atlas.depth[at][0],atlas.depth[at][1],atlas.side);
        camera::Vec3 p{};for (int k=0;k<3;++k) p[k]=basis.right[k]*u+basis.up[k]*w+basis.eye[k]*d;
        points.push_back(p);
    }
    return points;
}
struct Grid {
    unsigned n;double side;
    void splat(const ImpostorBasis& basis,camera::Vec3 p,std::vector<char>& mask) const {
        const double sx=(camera::dot(p,basis.right)/side+.5)*n-.5,sy=(.5-camera::dot(p,basis.up)/side)*n-.5;
        const int x0=std::max(0,int(std::ceil(sx-.71))),x1=std::min(int(n)-1,int(std::floor(sx+.71)));
        const int y0=std::max(0,int(std::ceil(sy-.71))),y1=std::min(int(n)-1,int(std::floor(sy+.71)));
        for (int y=y0;y<=y1;++y) for (int x=x0;x<=x1;++x) mask[std::size_t(y)*n+x]=1;
    }
    // Texels within which 99% of the union of both silhouettes agree.
    double disagreement(const std::vector<char>& reference,const std::vector<char>& shown) const {
        std::vector<double> distances;std::size_t covered=0;
        for (std::size_t at=0;at<reference.size();++at) {
            if (!reference[at] && !shown[at]) continue;
            ++covered;if (reference[at] && shown[at]) continue;
            const auto& other=reference[at]?shown:reference;
            const int x=int(at%n),y=int(at/n);double best=n;
            for (int r=1;r<int(n) && r<best;++r) for (int j=-r;j<=r;++j) for (int i=-r;i<=r;++i) {
                if (std::max(std::abs(i),std::abs(j))!=r) continue;
                const int px=x+i,py=y+j;
                if (px<0 || py<0 || px>=int(n) || py>=int(n) || !other[std::size_t(py)*n+px]) continue;
                best=std::min(best,std::hypot(double(i),double(j)));
            }
            distances.push_back(best);
        }
        const std::size_t allowed=covered/100;
        if (distances.size()<=allowed) return 0;
        std::nth_element(distances.begin(),distances.begin()+std::ptrdiff_t(allowed),distances.end(),std::greater<>());
        return distances[allowed];
    }
};
// The baked views themselves and the directions between them.
std::vector<ImpostorBasis> testDirections() {
    std::vector<ImpostorBasis> tests;const double degree=std::numbers::pi/180;
    for (unsigned v=0;v<kHemisphereViews;++v) tests.push_back(*hemisphereBasis(v));
    for (int i=0;i<8;++i) {
        tests.push_back(basisFor((i+.5)*45*degree,0));
        tests.push_back(basisFor(i*45*degree,22.5*degree));tests.push_back(basisFor((i+.5)*45*degree,22.5*degree));
        tests.push_back(basisFor(i*45*degree,57.5*degree));tests.push_back(basisFor((i+.5)*45*degree,57.5*degree));
    }
    for (int i=0;i<4;++i) tests.push_back(basisFor((i+.5)*90*degree,80*degree));
    return tests;
}
camera::Vec3 turn(camera::Vec3 p,double yaw) {
    const auto c=std::cos(yaw),s=std::sin(yaw);return {c*p[0]-s*p[1],s*p[0]+c*p[1],p[2]};
}
}
double measureImpostorViewError(const ImpostorAtlas& atlas) {
    if (!atlas.valid()) return -1;
    const Grid grid{atlas.resolution,atlas.side};const auto pixels=std::size_t(grid.n)*grid.n;
    std::vector<std::vector<camera::Vec3>> points(kHemisphereViews);
    for (unsigned v=0;v<kHemisphereViews;++v) points[v]=surface(atlas,v);
    double worst=0;std::vector<char> reference(pixels),shown(pixels);
    for (const auto& basis:testDirections()) {
        const auto selection=selectHemisphere(basis.eye);if (!selection) continue;
        std::fill(reference.begin(),reference.end(),0);
        for (const auto& cloud:points) for (const auto& p:cloud) grid.splat(basis,p,reference);
        for (unsigned i=0;i<4;++i) {
            if (!(selection->weights[i]>1e-3)) continue;
            std::fill(shown.begin(),shown.end(),0);
            for (const auto& p:points[selection->views[i]]) grid.splat(basis,p,shown);
            worst=std::max(worst,grid.disagreement(reference,shown)*atlas.side/grid.n);
        }
    }
    return worst;
}
double measureImpostorAgainstChildren(const ImpostorAtlas& parent,std::span<const ImpostorPlacement> children,
        const std::atomic_bool* cancel) {
    if (!parent.valid() || children.empty()) return -1;
    const Grid grid{parent.resolution,parent.side};const auto pixels=std::size_t(grid.n)*grid.n;
    std::vector<std::vector<camera::Vec3>> own(kHemisphereViews);
    for (unsigned v=0;v<kHemisphereViews;++v) own[v]=surface(parent,v);
    // Children share sources (a forest is a few models many times): unpack
    // each distinct atlas once.
    std::map<const ImpostorAtlas*,std::vector<std::vector<camera::Vec3>>> local;
    std::map<const ImpostorAtlas*,double> smallest;
    for (const auto& child:children) {
        if (!child.atlas || !child.atlas->valid()) return -1;
        auto [it,fresh]=smallest.try_emplace(child.atlas.get(),child.scale);
        if (!fresh) it->second=std::min(it->second,child.scale);
    }
    const double parentTexel=parent.side/parent.resolution;
    for (const auto& child:children) {
        auto& views=local[child.atlas.get()];
        if (views.empty()) {
            const double childTexel=child.atlas->side*smallest.at(child.atlas.get())/child.atlas->resolution;
            const auto stride=unsigned(std::max(1.0,std::floor(0.7*parentTexel/std::max(childTexel,1e-9))));
            views.resize(kHemisphereViews);for (unsigned v=0;v<kHemisphereViews;++v) views[v]=surface(*child.atlas,v,stride);
        }
    }
    double worst=0;std::vector<char> reference(pixels),shown(pixels);
    // The shader picks ONE contributing view per pixel by a dither weighted
    // like the selection. A texel only one light view misses reads as a
    // little transparency; it is a hole when half the weight misses it.
    std::vector<float> chance(pixels);std::vector<std::uint32_t> stamp(pixels);std::uint32_t id=0;
    const auto weighted=[&](const ImpostorBasis& basis,const std::vector<camera::Vec3>& points,double weight,
                            auto transform) {
        ++id;
        for (const auto& q:points) {
            const auto p=transform(q);
            const double sx=(camera::dot(p,basis.right)/grid.side+.5)*grid.n-.5,sy=(.5-camera::dot(p,basis.up)/grid.side)*grid.n-.5;
            const int x0=std::max(0,int(std::ceil(sx-.71))),x1=std::min(int(grid.n)-1,int(std::floor(sx+.71)));
            const int y0=std::max(0,int(std::ceil(sy-.71))),y1=std::min(int(grid.n)-1,int(std::floor(sy+.71)));
            for (int y=y0;y<=y1;++y) for (int x=x0;x<=x1;++x) {
                const auto at=std::size_t(y)*grid.n+x;
                if (stamp[at]!=id) {stamp[at]=id;chance[at]=std::min(1.0f,chance[at]+float(weight));}
            }
        }
    };
    const auto identity=[](camera::Vec3 p){return p;};
    for (const auto& basis:testDirections()) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return -1;
        const auto selection=selectHemisphere(basis.eye);if (!selection) continue;
        // What the finer level would show from here: every child reprojected
        // from the views IT would select, with its own yaw and dither weights.
        std::fill(chance.begin(),chance.end(),0.0f);
        for (const auto& child:children) {
            const auto mine=selectHemisphere(basis.eye,child.yaw);if (!mine) continue;
            const auto& views=local.at(child.atlas.get());
            const auto place=[&](camera::Vec3 q) {
                auto p=turn(q,child.yaw);
                for (int k=0;k<3;++k) p[k]=p[k]*child.scale+(child.centre[k]-parent.centre[k]);
                return p;
            };
            for (unsigned i=0;i<4;++i) if (mine->weights[i]>1e-3) weighted(basis,views[mine->views[i]],mine->weights[i],place);
        }
        for (std::size_t at=0;at<pixels;++at) reference[at]=chance[at]>=0.5f;
        std::fill(chance.begin(),chance.end(),0.0f);
        for (unsigned i=0;i<4;++i) if (selection->weights[i]>1e-3)
            weighted(basis,own[selection->views[i]],selection->weights[i],identity);
        for (std::size_t at=0;at<pixels;++at) shown[at]=chance[at]>=0.5f;
        worst=std::max(worst,grid.disagreement(reference,shown)*parent.side/grid.n);
    }
    return worst;
}
std::shared_ptr<const ImpostorAtlas> withMeasuredError(const std::shared_ptr<const ImpostorAtlas>& parent,
        std::span<const ImpostorPlacement> children,const std::atomic_bool* cancel) {
    if (!parent) return {};
    const double measured=measureImpostorAgainstChildren(*parent,children,cancel);
    if (measured<0) return {};
    auto copy=std::make_shared<ImpostorAtlas>(*parent);
    double below=0;
    for (const auto& child:children) below=std::max(below,impostorTotalError(*child.atlas)*child.scale);
    copy->error=below;copy->viewError=measured;
    return copy;
}

ImpostorStore::ImpostorStore(std::size_t byteBudget,std::size_t maxEntries) : budget_(byteBudget),maxEntries_(maxEntries) {
    if (!budget_ || !maxEntries_) throw std::invalid_argument("invalid impostor store budget");
}
std::size_t ImpostorStore::cost(const Entry& entry) { return 64+(entry.atlas?entry.atlas->bytes():0); }
const ImpostorStore::Entry* ImpostorStore::find(ImpostorKey key,std::uint64_t revision) const {
    const auto it=entries_.find(key);
    return it!=entries_.end() && it->second.revision==revision?&it->second:nullptr;
}
void ImpostorStore::touch(ImpostorKey key,double now) {
    if (const auto it=entries_.find(key);it!=entries_.end()) it->second.used=std::max(it->second.used,now);
}
bool ImpostorStore::put(ImpostorKey key,std::uint64_t revision,std::shared_ptr<const ImpostorAtlas> atlas,double now) {
    if (atlas && !atlas->valid()) return false;
    Entry entry{revision,std::move(atlas),now};
    if (cost(entry)>budget_) return false;
    erase(key);
    bytes_+=cost(entry);entries_.emplace(key,std::move(entry));
    evict(key);
    return true;
}
void ImpostorStore::erase(ImpostorKey key) {
    if (const auto it=entries_.find(key);it!=entries_.end()) {bytes_-=cost(it->second);entries_.erase(it);}
}
void ImpostorStore::clear() { entries_.clear();bytes_=0; }
void ImpostorStore::evict(ImpostorKey keep) {
    // Only the oldest few are ordered: a put evicts a handful, and sorting
    // the whole store for that was a frame-thread spike once it held thousands.
    while (bytes_>budget_ || entries_.size()>maxEntries_) {
        std::vector<std::pair<double,ImpostorKey>> order;order.reserve(entries_.size());
        for (const auto& [key,entry]:entries_) if (key!=keep) order.push_back({entry.used,key});
        if (order.empty()) return;
        const auto take=std::min<std::size_t>(order.size(),32);
        std::partial_sort(order.begin(),order.begin()+std::ptrdiff_t(take),order.end());
        for (std::size_t i=0;i<take;++i) {
            if (bytes_<=budget_ && entries_.size()<=maxEntries_) return;
            erase(order[i].second);
        }
    }
}

HierarchyCut selectImpostorHierarchy(const camera::ViewState& view,std::span<const ImpostorKey> roots,
        const std::function<HierarchyNodeInfo(ImpostorKey)>& info,std::span<const ImpostorKey> resident,
        std::span<const ImpostorKey> previous,const HierarchyOptions& options) {
    HierarchyCut cut;
    if (!view.valid() || !info) return cut;
    std::set<ImpostorKey> above; // strict ancestors of something drawable
    for (auto key:resident) while (key.level+1<kHierarchyLevels) {key=hierarchyParent(key);if (!above.insert(key).second) break;}
    const std::set<ImpostorKey> before(previous.begin(),previous.end());
    const double h=std::clamp(view.quality.hysteresis,0.0,0.45);
    const double scale=std::max(1.0,options.allowanceScale);
    const double allowance=view.quality.impostorErrorPx*scale;
    std::vector<ImpostorKey> pending(roots.rbegin(),roots.rend());
    while (!pending.empty()) {
        if (cut.visited>=options.maxVisits) {cut.budgetLimited=true;break;}
        const auto key=pending.back();pending.pop_back();++cut.visited;
        if (key.level<options.minLevel || key.level>=kHierarchyLevels) continue;
        const auto node=info(key);
        if (!node.visible || node.empty) {cut.invisible+=!node.visible;continue;}
        const bool refinable=key.level>options.minLevel;
        const auto push=[&] { if (refinable) {const auto c=hierarchyChildren(key);pending.insert(pending.end(),c.rbegin(),c.rend());} };
        // Could the finest level be accepted anywhere inside this node? Errors
        // scale with the cell; the most favourable descendant is the farthest
        // one, so test the smallest scale the node reaches, never its nearest.
        // Asked first: a node nothing below which can be drawn is not walked,
        // whatever else (density, horizon) would have sent us into it.
        const auto hopeless=[&](double errorMetres) {
            if (above.contains(key)) return false;
            const double finest=errorMetres/double(std::int64_t(1)<<(2*(key.level-options.minLevel)));
            double farthest=view.orthographicScale;
            if (!view.orthographic) {
                const double depth=camera::dot(camera::subtract(node.bounds.centre,view.position),camera::normalized(view.forward));
                farthest=view.projectionScale()/std::max(view.nearPlane,depth+node.bounds.radius);
            }
            return finest*farthest>allowance*(1+h);
        };
        if (hopeless(node.errorMetres)) {++cut.pruned;continue;}
        if (node.mustRefine) {push();continue;}
        const auto hemisphere=selectHemisphere(view.directionFrom(node.bounds.centre));
        if (!hemisphere) {
            // Below the baked hemisphere: the finer path owns it. A child can
            // only do better if some of this node lies below the eye.
            ++cut.below;
            if (view.orthographic || node.bounds.centre[2]-node.bounds.radius<view.position[2]) push();
            continue;
        }
        RepresentationCandidate proxy;
        proxy.kind=RepresentationKind::Aggregate;
        const double angular=node.viewErrorIncluded?0.0:node.bounds.radius*2*std::sin(hemisphere->maxAngle*.5);
        proxy.errorMetres=node.errorMetres+angular;
        proxy.residualDepth=node.bounds.radius*2;
        const auto metrics=representationMetrics(view,node.bounds,proxy);
        const double factor=before.contains(key)?1+h:1;
        const bool acceptable=metrics.errorPx<=allowance*factor &&
                              metrics.parallaxErrorPx<=view.quality.parallaxErrorPx*scale*factor;
        if (acceptable) {
            if (node.resident && cut.draws.size()<options.maxDraws) {cut.draws.push_back(key);continue;}
            if (node.resident) cut.budgetLimited=true;
            cut.wanted.push_back({key,metrics.projectedRadiusPx});
            if (above.contains(key)) push(); // children stay until this node is complete
            continue;
        }
        ++cut.coarse;
        if (!refinable) continue;
        if (hopeless(proxy.errorMetres)) {++cut.pruned;continue;}
        push();
    }
    return cut;
}
} // namespace engine::render

