#include "game/render/shadow_clipmap.hpp"
#include <chrono>
#include <fstream>
#include <nlohmann/json.hpp>

namespace game {
using world::shadow::Vec;
ShadowClipmap::~ShadowClipmap() { if (job_.valid()) job_.wait(); }

bool ShadowClipmap::setup(engine::Device& device) {
    // Dimensions are canonical asset metadata, not a readback of render meshes.
    std::ifstream input(device.assets()/"../generated/scene_models/manifest.json");
    const auto manifest=nlohmann::json::parse(input,nullptr,false);
    if (!manifest.is_object() || !manifest.contains("models")) {
        device.fail("shadow compiler requires the canonical model catalogue"); return false;
    }
    for (const auto& model:manifest["models"]) for (std::size_t i=0;i<profiles_.size();++i)
        if (model.value("name","")==world::decor::kModels[i]) {
            profiles_[i].width=model.value("width",1.0);
            profiles_[i].height=model.value("height",1.0);
        }
    SDL_GPUTextureCreateInfo info{};
    info.type=SDL_GPU_TEXTURETYPE_2D_ARRAY;
    info.format=SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
    info.usage=SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width=info.height=world::shadow::Tile::kSize;
    info.layer_count_or_depth=4;info.num_levels=1;
    texture_=device.makeTexture(info);
    SDL_GPUSamplerCreateInfo sample{};
    sample.min_filter=sample.mag_filter=SDL_GPU_FILTER_NEAREST;
    sample.address_mode_u=sample.address_mode_v=sample.address_mode_w=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_=device.makeSampler(sample);
    if (!texture_ || !sampler_) return false;
    engine::Device::Uploader upload(device);
    for (int layer=0;layer<4;++layer)
        if (!upload.refillRegion(texture_.get(),resident_[layer].tile.pixels.data(),0,0,info.width,info.height,
                                 sizeof(world::shadow::Texel),std::uint32_t(layer))) return false;
    return upload.finish();
}

bool ShadowClipmap::dirty(const Result& r,const world::ecology::Delta& delta) const {
    for (const auto& [region,revision]:delta.regions) {
        if (revision<=r.revision) continue;
        const double x=double(region.first)*128,y=double(region.second)*128;
        if (x<r.tile.origin.x+r.radius && x+128>r.tile.origin.x-r.radius &&
            y<r.tile.origin.y+r.radius && y+128>r.tile.origin.y-r.radius) return true;
    }
    return false;
}

ShadowClipmap::Result ShadowClipmap::compile(world::WorldBuilder::Snapshot world,
    std::shared_ptr<const world::ecology::Delta> delta,Result result,
    const std::array<Profile,world::decor::kModels.size()>& profiles) {
    auto query=world->field();
    auto& tile=result.tile;
    // A bounded terrain source grid, independent of resident render patches.
    // Coarse levels represent large terrain forms; near levels preserve relief.
    constexpr int n=48;
    std::array<Vec,(n+1)*(n+1)> grid;
    std::map<world::ecology::Key,world::ecology::Cell> ecology;
    const double width=double(world->worldMap().width)*generation::kMetresPerCell;
    const double height=double(world->worldMap().height)*generation::kMetresPerCell;
    for (int y=0;y<=n;++y) for (int x=0;x<=n;++x) {
        const double wx=std::clamp(tile.origin.x-result.radius+x*(2*result.radius/n),0.0,width);
        const double wy=std::clamp(tile.origin.y-result.radius+y*(2*result.radius/n),0.0,height);
        const core::WorldPos p{core::Fixed::fromDoubleForContent(wx),core::Fixed::fromDoubleForContent(wy)};
        Vec v{wx,wy,query.heightAt(p).toDouble()};
        grid[std::size_t(y*(n+1)+x)]=v;
        if (result.level>=2 && v.z>=0) {
            auto [cell,fresh]=ecology.try_emplace(world::ecology::key(wx,wy));
            if (fresh) cell->second=world->ecologyAt(wx,wy,query,*delta);
            const auto& c=cell->second;
            // A blanket for the canopy the coarse levels cannot place tree by
            // tree. Its lobes overlap almost twice over (radius 0.9 of the
            // grid step, not half of it): touching spheres made the crown's
            // depth a row of humps, and whenever this level stood in for a
            // finer one being recompiled - every few frames, with the camera
            // moving - the ground under it was shaded in even stripes. The
            // density is spread over the overlap so the shade is the same.
            const double step=2*result.radius/n;
            if (c.canopy>0.1f) tile.ellipsoid(v+Vec{0,0,12},
                {step*0.9,step*0.9,8},c.canopy*0.12f*0.42f);
        }
    }
    for (int y=0;y<n;++y) for (int x=0;x<n;++x) {
        const auto at=std::size_t(y*(n+1)+x);
        tile.triangle(grid[at],grid[at+1],grid[at+n+2]);
        tile.triangle(grid[at],grid[at+n+2],grid[at+n+1]);
    }
    if (result.level<2) {
        // Match the placement compiler's stable candidates, including deltas;
        // never depend on camera visibility or on geometry compilation finishing.
        const auto floor=[](double x) { return std::int64_t(std::floor(x/128))*128; };
        const auto minX=floor(tile.origin.x-result.radius),minY=floor(tile.origin.y-result.radius);
        const auto maxX=floor(tile.origin.x+result.radius),maxY=floor(tile.origin.y+result.radius);
        for (auto y=minY;y<=maxY;y+=128) for (auto x=minX;x<=maxX;x+=128) {
            if (x<0 || y<0 || double(x)>=width || double(y)>=height) continue;
            const auto scatter=world->scatter({x,y,x+128,y+128},*delta,query);
            for (const auto& object:scatter.objects) {
                if (object.model>=profiles.size()) continue;
                const auto& profile=profiles[object.model];
                const double h=profile.height*object.scale,w=profile.width*object.scale;
                const Vec root{object.x,object.y,object.z};
                if (world::decor::treeModel(object.model)) {
                    // Trunk/crown volumes are authored semantic proxies. Detailed
                    // imported branch skeletons can replace this source without
                    // changing the cache, projection or receiver shaders.
                    tile.ellipsoid(root+Vec{0,0,h*0.4},{w*0.035,w*0.035,h*0.4},-1);
                    const bool conifer=world::decor::coniferModel(object.model);
                    const int lobes=conifer?3:4;
                    for (int i=0;i<lobes;++i) {
                        const double angle=object.yaw+i*6.28318530718/lobes;
                        const double radial=conifer?0:w*0.19;
                        const double z=conifer?h*(0.38+i*0.19):h*(0.62+0.05*(i%2));
                        const double r=conifer?w*(0.46-i*0.1):w*0.32;
                        tile.ellipsoid(root+Vec{std::cos(angle)*radial,std::sin(angle)*radial,z},
                            {r,r,h*(conifer?0.22:0.30)},profile.density);
                    }
                } else if (object.model==2 || object.model==3 || (object.model>=22 && object.model<=24) ||
                           (object.model>=world::decor::kFirstRock2 && object.model<=world::decor::kLastRock2)) {
                    const bool solid=object.model==3 || (object.model>=world::decor::kFirstRock2 && object.model<=world::decor::kLastRock2);
                    tile.ellipsoid(root+Vec{0,0,h*0.5},{w*0.5,w*0.5,h*0.5},solid?-1:profile.density);
                }
            }
        }
    }
    return result;
}

bool ShadowClipmap::update(engine::Device& device,engine::Scene& scene,std::array<float,3> direction,bool enabled) {
    const world::shadow::Basis basis({direction[0],direction[1],direction[2]});
    scene.shadowSun[0]=float(basis.sun.x);scene.shadowSun[1]=float(basis.sun.y);scene.shadowSun[2]=float(basis.sun.z);
    scene.shadowSun[3]=enabled && basis.sun.z>0.12?1.0f:0.0f;
    const auto delta=world_->ecology().read();
    for (int i=0;i<4;++i) if (valid_[i] &&
        (world::shadow::dot(resident_[i].tile.basis.sun,basis.sun)<0.99999 || dirty(resident_[i],*delta))) valid_[i]=false;
    if (job_.valid() && job_.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        Result result;
        try { result=job_.get(); }
        catch (const std::exception& e) { device.fail(std::string("shadow compiler: ")+e.what());return false; }
        if (!dirty(result,*delta) && world::shadow::dot(result.tile.basis.sun,basis.sun)>0.99999) {
            engine::Device::Uploader upload(device);
            if (!upload.refillRegion(texture_.get(),result.tile.pixels.data(),0,0,world::shadow::Tile::kSize,
                world::shadow::Tile::kSize,sizeof(world::shadow::Texel),std::uint32_t(result.level)) || !upload.finish()) return false;
            valid_[result.level]=true;
            resident_[result.level]=std::move(result);
        }
    }
    if (scene.shadowSun[3]>0 && !job_.valid()) {
        for (int offset=0;offset<4;++offset) {
            const int level=(next_+offset)%4;
            const double span=128.0*std::pow(4.0,level);
            const auto* eye=engine::cullEye(scene);
            const Vec focus{eye[0],eye[1],eye[2]};
            if (valid_[level]) {
                const auto local=basis.project(focus-resident_[level].tile.origin);
                if (std::max(std::abs(local.x),std::abs(local.y))<span*0.125) continue;
            }
            Result wanted;
            wanted.level=level;wanted.tile.span=span;wanted.tile.basis=basis;wanted.revision=delta->revision;
            const Vec light=basis.project(focus);
            const double snap=span/16;
            wanted.tile.origin=basis.right*(std::floor(light.x/snap)*snap)+
                basis.up*(std::floor(light.y/snap)*snap)+basis.sun*light.z;
            wanted.radius=span/std::max(0.25,basis.sun.z)*0.75+48;
            job_=std::async(std::launch::async,compile,world_,delta,std::move(wanted),profiles_);
            next_=(level+1)%4;
            break; // one worker, one completed upload per frame, fixed memory
        }
    }
    for (int i=0;i<4;++i) {
        const auto& tile=resident_[i].tile;
        scene.shadowClip[i][0]=float(tile.origin.x);scene.shadowClip[i][1]=float(tile.origin.y);
        scene.shadowClip[i][2]=float(tile.origin.z);scene.shadowClip[i][3]=valid_[i]?float(tile.span):0;
    }
    return true;
}
} // namespace game

