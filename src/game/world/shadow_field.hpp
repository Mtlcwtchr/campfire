#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace world::shadow {
struct Vec {
    double x=0,y=0,z=0;
    Vec operator+(Vec b) const { return {x+b.x,y+b.y,z+b.z}; }
    Vec operator-(Vec b) const { return {x-b.x,y-b.y,z-b.z}; }
    Vec operator*(double s) const { return {x*s,y*s,z*s}; }
};
inline double dot(Vec a,Vec b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
inline Vec normalized(Vec v) { const double n=std::sqrt(dot(v,v)); return n>1e-9?v*(1/n):Vec{0,0,1}; }
inline Vec cross(Vec a,Vec b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
struct Basis {
    Vec sun, right, up;
    explicit Basis(Vec direction):sun(normalized(direction)) {
        right=normalized(std::abs(sun.z)>0.999?Vec{1,0,0}:Vec{-sun.y,sun.x,0});
        up=cross(sun,right);
    }
    Vec project(Vec p) const { return {dot(p,right),dot(p,up),dot(p,sun)}; }
};
// Two independent depths: opaque surfaces must not be lost behind a transmitting crown.
struct Texel { float opaque=-1e20f, crown=-1e20f, optical=0, terrain=-1e20f; };
struct Tile {
    static constexpr int kSize=128;
    Vec origin;
    Basis basis{{-0.55,-0.55,0.63}};
    double span=128;
    std::vector<Texel> pixels=std::vector<Texel>(kSize*kSize);
    Vec pixel(Vec world) const {
        Vec p=basis.project(world-origin);
        return {(p.x/span+0.5)*kSize,(p.y/span+0.5)*kSize,p.z};
    }
    void triangle(Vec a,Vec b,Vec c) {
        a=pixel(a);b=pixel(b);c=pixel(c);
        const double area=(b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);
        if (std::abs(area)<1e-9) return;
        const int x0=std::max(0,int(std::floor(std::min({a.x,b.x,c.x}))));
        const int y0=std::max(0,int(std::floor(std::min({a.y,b.y,c.y}))));
        const int x1=std::min(kSize-1,int(std::ceil(std::max({a.x,b.x,c.x}))));
        const int y1=std::min(kSize-1,int(std::ceil(std::max({a.y,b.y,c.y}))));
        for (int y=y0;y<=y1;++y) for (int x=x0;x<=x1;++x) {
            const double px=x+0.5-a.x,py=y+0.5-a.y;
            const double u=(px*(c.y-a.y)-py*(c.x-a.x))/area;
            const double v=((b.x-a.x)*py-(b.y-a.y)*px)/area;
            if (u<0 || v<0 || u+v>1) continue;
            auto& t=pixels[std::size_t(y*kSize+x)];
            t.terrain=std::max(t.terrain,float(a.z+u*(b.z-a.z)+v*(c.z-a.z)));
        }
    }
    // Canonical axis-aligned ellipsoid, evaluated along parallel light rays.
    // No triangles, UVs or final-mesh topology are needed for an occluder.
    void ellipsoid(Vec centre,Vec radii,float density) {
        if (std::min({radii.x,radii.y,radii.z})<=0) return;
        const Vec p=basis.project(centre-origin);
        const double extent=std::max({radii.x,radii.y,radii.z});
        const int x0=std::max(0,int(std::floor((p.x-extent)/span*kSize+kSize*0.5)));
        const int y0=std::max(0,int(std::floor((p.y-extent)/span*kSize+kSize*0.5)));
        const int x1=std::min(kSize-1,int(std::ceil((p.x+extent)/span*kSize+kSize*0.5)));
        const int y1=std::min(kSize-1,int(std::ceil((p.y+extent)/span*kSize+kSize*0.5)));
        const auto scaled=[&](Vec v) { return Vec{v.x/radii.x,v.y/radii.y,v.z/radii.z}; };
        const Vec d=scaled(basis.sun);
        const double aa=dot(d,d);
        for (int y=y0;y<=y1;++y) for (int x=x0;x<=x1;++x) {
            const Vec offset=basis.right*((x+0.5)/kSize*span-span*0.5-p.x)+
                             basis.up*((y+0.5)/kSize*span-span*0.5-p.y);
            const Vec o=scaled(offset);
            const double bb=dot(o,d),cc=dot(o,o)-1,disc=bb*bb-aa*cc;
            if (disc<=0) continue;
            const double chord=2*std::sqrt(disc)/aa;
            const float depth=float(p.z+(-bb+std::sqrt(disc))/aa);
            auto& texel=pixels[std::size_t(y*kSize+x)];
            if (density<0) texel.opaque=std::max(texel.opaque,depth);
            else {
                texel.crown=std::max(texel.crown,depth);
                texel.optical=std::min(12.0f,texel.optical+float(chord)*density);
            }
        }
    }
    float visibility(Vec p,double bias=0.1) const {
        const Vec q=pixel(p);
        const int x=int(std::floor(q.x)),y=int(std::floor(q.y));
        if (x<0 || y<0 || x>=kSize || y>=kSize) return 1;
        const auto& t=pixels[std::size_t(y*kSize+x)];
        if (q.z+bias<t.opaque || q.z+bias<t.terrain) return 0;
        return q.z+bias<t.crown?std::exp(-t.optical):1;
    }
};
} // namespace world::shadow

