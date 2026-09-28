#include "engine/render/recursive_impostor.hpp"
#include <algorithm>
#include <limits>
#include <tuple>

namespace engine::render {
bool ImpostorAtlas::valid() const {
    const auto count=std::size_t(resolution)*resolution*kHemisphereViews;
    return resolution>=4 && resolution<=512 && std::isfinite(side+error) && side>0 && error>=0 &&
        colour.size()==count && normal.size()==count && depth.size()==count;
}
std::size_t ImpostorAtlas::bytes() const {
    std::size_t n=(colour.size()+normal.size()+depth.size())*sizeof(ImpostorPixel);
    for (const auto* mips:{&colourMips,&normalMips}) for (const auto& mip:*mips) n+=mip.size()*sizeof(ImpostorPixel);
    return n;
}
namespace {
bool cancelled(const std::atomic_bool* flag) { return flag && flag->load(std::memory_order_relaxed); }
camera::Vec3 rotate(camera::Vec3 p,double yaw) {
    const auto c=std::cos(yaw),s=std::sin(yaw);return {c*p[0]-s*p[1],s*p[0]+c*p[1],p[2]};
}
std::uint8_t byte(double v) { return std::uint8_t(std::clamp(std::lround(v),0l,255l)); }
}
void buildImpostorMips(ImpostorAtlas& atlas) {
    atlas.colourMips.clear();atlas.normalMips.clear();
    auto colours=atlas.colour,normals=atlas.normal;
    for (unsigned side=atlas.resolution;side>1;side/=2) {
        const auto next=side/2;
        std::vector<ImpostorPixel> c(std::size_t(next)*next*21),n(c.size());
        for (unsigned view=0;view<21;++view) for (unsigned y=0;y<next;++y) for (unsigned x=0;x<next;++x) {
            double alpha=0;camera::Vec3 rgb{},normal{};
            for (unsigned j=0;j<2;++j) for (unsigned i=0;i<2;++i) {
                const auto at=std::size_t(view)*side*side+(2*y+j)*side+2*x+i;
                const double a=colours[at][3];alpha+=a;
                for (int k=0;k<3;++k) {rgb[k]+=colours[at][k]*a;normal[k]+=(normals[at][k]/127.5-1)*a;}
            }
            const auto at=std::size_t(view)*next*next+y*next+x;
            const auto unit=camera::normalized(normal);
            for (int k=0;k<3;++k) {c[at][k]=byte(rgb[k]/std::max(alpha,1.0));n[at][k]=byte((unit[k]*.5+.5)*255);}
            c[at][3]=n[at][3]=byte(alpha*.25);
        }
        atlas.colourMips.push_back(c);atlas.normalMips.push_back(n);
        colours=std::move(c);normals=std::move(n);
    }
}
namespace {
std::shared_ptr<const ImpostorAtlas> combine(std::span<const ImpostorPlacement> children,
        unsigned resolution,const std::atomic_bool* cancel) {
    if (children.empty() || cancelled(cancel)) return {};
    camera::Vec3 low{1e30,1e30,1e30},high{-1e30,-1e30,-1e30};
    for (const auto& child:children) {
        const auto radius=child.atlas->side*child.scale*.5;
        for (int k=0;k<3;++k) {low[k]=std::min(low[k],child.centre[k]-radius);high[k]=std::max(high[k],child.centre[k]+radius);}
    }
    auto result=std::make_shared<ImpostorAtlas>();result->resolution=resolution;
    for (int k=0;k<3;++k) result->centre[k]=(low[k]+high[k])*.5;
    // Enclose child spheres, not just their centres or the ground-cell square.
    for (const auto& child:children) {
        const auto delta=camera::subtract(child.centre,result->centre);
        result->side=std::max(result->side,2*std::sqrt(camera::dot(delta,delta))+child.atlas->side*child.scale);
        result->members+=child.atlas->members;
        result->levels=std::max(result->levels,child.atlas->levels+1);
    }
    result->side*=1.01;
    const auto pixels=std::size_t(resolution)*resolution;
    result->colour.resize(pixels*21);result->normal.resize(pixels*21);result->depth.resize(pixels*21);
    for (unsigned view=0;view<21;++view) {
        if (cancelled(cancel)) return {};
        const auto basis=*hemisphereBasis(view);
        std::vector<double> zbuffer(pixels,-std::numeric_limits<double>::infinity());
        for (const auto& child:children) {
            if (cancelled(cancel)) return {};
            const auto& atlas=*child.atlas;
            const auto eye=rotate(basis.eye,-child.yaw);
            unsigned sourceView=0;double best=-2;
            for (unsigned v=0;v<21;++v) {
                const double alignment=camera::dot(eye,hemisphereBasis(v)->eye);
                if (alignment>best) {best=alignment;sourceView=v;}
            }
            const auto source=*hemisphereBasis(sourceView);
            const double sourceCell=atlas.side/atlas.resolution*child.scale;
            const double outputCell=result->side/resolution;
            result->error=std::max(result->error,atlas.error*child.scale+
                atlas.side*child.scale*std::sqrt(std::max(0.0,(1-best)*.5))+2*(sourceCell+outputCell));
            const auto sourcePixels=std::size_t(atlas.resolution)*atlas.resolution;
            for (unsigned y=0;y<atlas.resolution;++y) for (unsigned x=0;x<atlas.resolution;++x) {
                const auto at=sourceView*sourcePixels+y*atlas.resolution+x;
                const auto colour=atlas.colour[at];if (colour[3]<51 || atlas.depth[at][3]<51) continue;
                const double depth=impostorDepth(atlas.depth[at][0],atlas.depth[at][1],atlas.side);
                const double u=((x+.5)/atlas.resolution-.5)*atlas.side,v=(.5-(y+.5)/atlas.resolution)*atlas.side;
                camera::Vec3 p{},normal{};
                for (int k=0;k<3;++k) {p[k]=source.right[k]*u+source.up[k]*v+source.eye[k]*depth;normal[k]=atlas.normal[at][k]/127.5-1;}
                p=rotate(p,child.yaw);normal=rotate(camera::normalized(normal),child.yaw);
                for (int k=0;k<3;++k) p[k]=p[k]*child.scale+(child.centre[k]-result->centre[k]);
                const double sx=(camera::dot(p,basis.right)/result->side+.5)*resolution-.5;
                const double sy=(.5-camera::dot(p,basis.up)/result->side)*resolution-.5;
                const double z=camera::dot(p,basis.eye);
                const double radius=std::max(.71,sourceCell/outputCell*.71);
                const int x0=std::max(0,int(std::ceil(sx-radius))),x1=std::min(int(resolution)-1,int(std::floor(sx+radius)));
                const int y0=std::max(0,int(std::ceil(sy-radius))),y1=std::min(int(resolution)-1,int(std::floor(sy+radius)));
                ImpostorPixel pigment{},encoded{};
                for (int k=0;k<3;++k) {pigment[k]=byte(colour[k]*child.tint);encoded[k]=byte((normal[k]*.5+.5)*255);}
                pigment[3]=encoded[3]=colour[3];
                for (int iy=y0;iy<=y1;++iy) for (int ix=x0;ix<=x1;++ix) {
                    const auto dest=std::size_t(iy)*resolution+ix,target=view*pixels+dest;
                    if (z<zbuffer[dest]-1e-9) continue;
                    if (std::abs(z-zbuffer[dest])<=1e-9 && std::tie(pigment,encoded)<=std::tie(result->colour[target],result->normal[target])) continue;
                    zbuffer[dest]=z;result->colour[target]=pigment;result->normal[target]=encoded;
                }
            }
        }
        for (std::size_t at=0;at<pixels;++at) {
            const auto target=view*pixels+at;
            const double value=std::isfinite(zbuffer[at])?zbuffer[at]/result->side+.5:.5;
            const auto code=std::uint16_t(std::clamp(std::lround(value*65535),0l,65535l));
            result->depth[target]={std::uint8_t(code>>8),std::uint8_t(code&255),std::uint8_t(view),result->colour[target][3]};
        }
    }
    buildImpostorMips(*result);
    return result;
}
std::shared_ptr<const ImpostorAtlas> recurse(std::vector<ImpostorPlacement> children,
        const ImpostorBakeOptions& options,unsigned depth,const std::atomic_bool* cancel) {
    if (cancelled(cancel)) return {};
    if (children.size()<=options.leafMembers || depth>=options.maxDepth) return combine(children,options.resolution,cancel);
    std::stable_sort(children.begin(),children.end(),[depth](const auto& a,const auto& b) {
        const auto axis=depth%2;return a.centre[axis]!=b.centre[axis]?a.centre[axis]<b.centre[axis]:a.centre[1-axis]<b.centre[1-axis];
    });
    std::vector<ImpostorPlacement> parents;
    const auto group=(children.size()+options.leafMembers-1)/options.leafMembers;
    for (std::size_t i=0;i<children.size();i+=group) {
        auto baked=recurse({children.begin()+i,children.begin()+std::min(children.size(),i+group)},options,depth+1,cancel);
        if (!baked) return {};
        parents.push_back({baked,baked->centre});
    }
    return combine(parents,options.resolution,cancel);
}
}
std::shared_ptr<const ImpostorAtlas> bakeRecursiveImpostor(std::span<const ImpostorPlacement> members,
        const ImpostorBakeOptions& options,const std::atomic_bool* cancel) {
    if (members.empty() || members.size()>options.maxMembers || options.leafMembers<2 || options.maxDepth>4 ||
        options.resolution<4 || options.resolution>128 || (options.resolution&(options.resolution-1))) return {};
    for (const auto& member:members) {
        if (!member.atlas || !member.atlas->valid() || !std::isfinite(member.scale+member.yaw+member.tint) ||
            member.scale<=0 || member.scale>16 || member.tint<0 || member.tint>4 || member.atlas->side*member.scale>1e6) return {};
        for (double v:member.centre) if (!std::isfinite(v) || std::abs(v)>1e9) return {};
    }
    return recurse({members.begin(),members.end()},options,0,cancel);
}
} // namespace engine::render
