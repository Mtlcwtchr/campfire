#include "game/generation/hybrid_terrain.hpp"
#include "game/generation/mountain_shape.hpp"
#include "game/generation/world_map_gen.hpp"
#include "engine/core/hash.hpp"
#include "engine/core/rng.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>

namespace generation {
namespace {
using core::Fixed;
using nlohmann::json;
constexpr std::array<const char*, 8> kFamilies{{"alpine_peaks", "glacial_highlands", "volcanic",
    "plateaus_canyons", "folded_ranges", "arid_ranges", "old_massifs", "rock_towers_karst"}};
constexpr int kSeaBed = HybridTerrain::kSeaBedMetres;

LandformFamily familyOf(const std::string& name) {
    const auto it = std::find(kFamilies.begin(), kFamilies.end(), name);
    if (it == kFamilies.end()) throw std::runtime_error("unknown terrain family: " + name);
    return static_cast<LandformFamily>(it - kFamilies.begin());
}
Fixed fade(Fixed t) {
    t = core::saturate(t);
    return t*t*t*(t*(t*6 - Fixed::fromInt(15)) + Fixed::fromInt(10));
}
std::int64_t floorDiv(std::int64_t n, std::int64_t d) {
    const auto q = n/d, r = n%d;
    return q - (r < 0);
}
Fixed spline(Fixed a, Fixed b, Fixed c, Fixed d, Fixed t) {
    return (b*2 + (c-a)*t + (a*2-b*5+c*4-d)*t*t + (b*3-a-c*3+d)*t*t*t)/2;
}
std::uint64_t atSeed(std::uint64_t seed, std::int64_t x, std::int64_t y) {
    return core::splitmix64(seed ^ core::splitmix64(static_cast<std::uint64_t>(x)) ^
                           (core::splitmix64(static_cast<std::uint64_t>(y)) << 1));
}
Fixed noise(std::uint64_t seed, Fixed x, Fixed y, int wave) {
    const auto side = Fixed::fromInt(wave);
    const auto ix = floorDiv(x.raw, side.raw), iy = floorDiv(y.raw, side.raw);
    const auto u = fade((x-Fixed::fromInt(ix*wave))/side);
    const auto v = fade((y-Fixed::fromInt(iy*wave))/side);
    const auto at = [&](int dx, int dy) {
        return Fixed::ratio(static_cast<std::int64_t>(atSeed(seed,ix+dx,iy+dy)&65535),32768)-core::kOne;
    };
    return core::lerp(core::lerp(at(0,0),at(1,0),u),core::lerp(at(0,1),at(1,1),u),v);
}
Fixed fractal(std::uint64_t seed, Fixed x, Fixed y, LandformFamily family, bool fine) {
    const bool ridged = family != LandformFamily::Old && family != LandformFamily::Glacial;
    const int firstWave = fine ? 512 : 1536;
    Fixed sum, weight = core::kOne, total;
    for (int octave = 0; octave != 4; ++octave) {
        Fixed n = noise(seed+octave*71,x,y,firstWave >> octave);
        if (ridged) n = core::kOne - core::abs(n)*2;
        // The preceding large shape modulates smaller ridges: not a uniform
        // roughness blanket. All frequencies are baked once, never per frame.
        sum += n*weight;
        total += weight;
        weight *= family == LandformFamily::Old ? Fixed::ratio(2,5) : Fixed::ratio(1,2);
    }
    return sum/total;
}
Fixed massif(std::uint64_t seed,Fixed x,Fixed y,int radius,LandformFamily family,bool branched) {
    const auto r=Fixed::fromInt(radius);
    const auto bend=noise(seed^0x51aa,x,core::kZero,radius/3)*r/12;
    const auto u=x/(r*Fixed::ratio(9,10)),v=(y-bend)/(r*Fixed::ratio(7,10));
    const auto body=fade(core::kOne-core::sqrt(u*u+v*v));
    const auto along=fade(core::kOne-core::abs(u));
    if (family==LandformFamily::Old)
        return body*(Fixed::ratio(9,10)+noise(seed,x,y,2800)/10);
    if (family==LandformFamily::Folded) {
        Fixed ridges;
        for (int n=-1;n<=1;++n) {
            const auto ridge=fade(core::kOne-core::abs(v-Fixed::ratio(n*55,100))/Fixed::ratio(38,100));
            ridges=std::max(ridges,ridge*(n==0 ? core::kOne : Fixed::ratio(7,10)));
        }
        return along*ridges*Fixed::ratio(4,5)+body/5;
    }
    if (family==LandformFamily::Glacial) {
        // A broad U-shaped floor between two continuous mountain walls.
        const auto left=fade(core::kOne-core::abs(v-Fixed::ratio(1,2))/Fixed::ratio(2,5));
        const auto right=fade(core::kOne-core::abs(v+Fixed::ratio(1,2))/Fixed::ratio(9,20));
        return along*std::max(left,right)*Fixed::ratio(9,10)+body/10;
    }
    const auto spine=mountains::crest(y-bend,r*Fixed::ratio(7,20))*along;
    const auto saddles=Fixed::ratio(4,5)+noise(seed^0x6a12,x,core::kZero,radius/4)/5;
    if (branched) {
        // The branching positive skeleton belongs to ONE sharp massif with a
        // shared body, not every elevated region or every mountain family.
        return body*Fixed::ratio(3,5)+mountains::sample(seed,x,y).height()*Fixed::ratio(1,2);
    }
    return body/4+spine*saddles*Fixed::ratio(3,4);
}
Fixed support(const LandformPatch& p, Fixed x, Fixed y) {
    const Fixed radius = Fixed::fromInt(p.radius);
    const Fixed u = (x-Fixed::fromInt(p.originX+p.radius))/radius;
    const Fixed v = (y-Fixed::fromInt(p.originY+p.radius))/radius;
    // Flat core, zero value AND derivative at the circular perimeter.
    return fade((core::kOne-u*u-v*v)/Fixed::ratio(3,4));
}
Fixed volcanicDeposits(const LandformPatch& p, Fixed x, Fixed y) {
    const auto envelope=support(p,x,y);
    if (envelope<=core::kZero) return {};
    // The saved patch identity drives both deposited relief and ground cover.
    // Broad lobes and finer ash patches fade out before the bounded footprint;
    // no per-page/LOD randomness and no hard circular material border.
    const auto seed=atSeed(0x415348ull+p.freshness,p.originX,p.originY);
    const auto dx=x-Fixed::fromInt(p.originX+p.radius),dy=y-Fixed::fromInt(p.originY+p.radius);
    const auto r=core::hypot(dx,dy)/Fixed::fromInt(p.radius);
    const auto scatter=noise(seed,x,y,1200)*Fixed::ratio(1,5)+noise(seed^0x17,x,y,320)*Fixed::ratio(2,25);
    return envelope*fade((core::kOne-r+scatter)/Fixed::ratio(2,5));
}
Fixed patchHeight(const LandformPatch& p, Fixed x, Fixed y) {
    const Fixed u = (x-Fixed::fromInt(p.originX))/Fixed::fromInt(HybridTerrain::kSampleMetres);
    const Fixed v = (y-Fixed::fromInt(p.originY))/Fixed::fromInt(HybridTerrain::kSampleMetres);
    if (u < core::kZero || v < core::kZero || u >= Fixed::fromInt(p.side-1) ||
        v >= Fixed::fromInt(p.side-1)) return {};
    const auto ix = static_cast<std::size_t>(u.toInt()), iy = static_cast<std::size_t>(v.toInt());
    const auto at = [&](std::size_t dx, std::size_t dy) { return Fixed::ratio(p.delta[(iy+dy)*p.side+ix+dx],10); };
    return core::lerp(core::lerp(at(0,0),at(1,0),u-Fixed::fromInt(ix)),
                      core::lerp(at(0,1),at(1,1),u-Fixed::fromInt(ix)),v-Fixed::fromInt(iy));
}
Fixed cellHeight(const WorldCell& c) {
    return Fixed::fromInt(c.sea ? kSeaBed : c.elevation*kMetresPerElevationStep);
}
Fixed baseAt(const WorldMapData& world, Fixed x, Fixed y) {
    const Fixed u = x/Fixed::fromInt(kMetresPerCell), v = y/Fixed::fromInt(kMetresPerCell);
    const auto ix=u.toInt(), iy=v.toInt();
    const auto at = [&](int dx,int dy) {
        return cellHeight(world.at({static_cast<int>(std::clamp<std::int64_t>(ix+dx,0,world.width-1)),
                                    static_cast<int>(std::clamp<std::int64_t>(iy+dy,0,world.height-1))}));
    };
    return core::lerp(core::lerp(at(0,0),at(1,0),u-Fixed::fromInt(ix)),
                      core::lerp(at(0,1),at(1,1),u-Fixed::fromInt(ix)),v-Fixed::fromInt(iy));
}

struct CatalogEntry { std::string id; LandformFamily family; std::filesystem::path metadata; };
std::vector<CatalogEntry> catalog(const std::filesystem::path& root, std::vector<std::string>& diagnostics) {
    std::vector<CatalogEntry> entries;
    try {
        std::ifstream in(root/"index.json");
        if (!in) throw std::runtime_error("terrain index unavailable: " + (root/"index.json").string());
        json j; in >> j;
        std::set<std::string> ids;
        for (const auto& item : j.at("regions")) {
            const auto id = item.at("id").get<std::string>();
            if (id.empty() || id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos ||
                !ids.insert(id).second) throw std::runtime_error("invalid/duplicate terrain reference ID");
            entries.push_back({id,familyOf(item.at("family").get<std::string>()),root/id/"metadata.json"});
        }
        std::sort(entries.begin(),entries.end(),[](const auto& a,const auto& b) { return a.id<b.id; });
    } catch (const std::exception& e) {
        diagnostics.push_back(std::string(e.what()) + "; using analytic landforms where needed");
        entries.clear();
    }
    return entries;
}

std::shared_ptr<const TerrainReference> reference(const CatalogEntry& entry) {
    // Generation workers share decoded immutable DEMs. No file lookup, mutex or
    // resource dependency remains in HeightField or a saved world.
    static std::mutex mutex;
    static std::map<std::filesystem::path,std::shared_ptr<const TerrainReference>> cache;
    std::lock_guard lock(mutex);
    auto& result = cache[entry.metadata];
    if (!result) {
        auto loaded = loadTerrainReference(entry.metadata);
        if (loaded.id != entry.id || loaded.family != entry.family)
            throw std::runtime_error("terrain index/metadata mismatch: " + entry.id);
        result = std::make_shared<const TerrainReference>(std::move(loaded));
    }
    return result;
}

void updateCoast(WorldMapData& world) {
    const auto count=world.cells.size();
    world.distanceToCoast.assign(count,-1);
    std::deque<std::size_t> queue;
    for (int y=0;y<world.height;++y) for (int x=0;x<world.width;++x) {
        const auto i=static_cast<std::size_t>(y)*world.width+x;
        for (int dir : core::kCardinalDirections) {
            const auto n=core::neighbour({x,y},dir);
            if (!world.inBounds(n) || world.at(n).sea!=world.cells[i].sea) {
                world.distanceToCoast[i]=0; queue.push_back(i); break;
            }
        }
    }
    while (!queue.empty()) {
        const auto i=queue.front(); queue.pop_front();
        const core::TilePos p{static_cast<int>(i%world.width),static_cast<int>(i/world.width)};
        for (int dir : core::kCardinalDirections) {
            const auto n=core::neighbour(p,dir);
            if (!world.inBounds(n)) continue;
            const auto j=static_cast<std::size_t>(n.y)*world.width+n.x;
            if (world.distanceToCoast[j]>=0) continue;
            world.distanceToCoast[j]=world.distanceToCoast[i]+1; queue.push_back(j);
        }
    }
}
} // namespace

const char* landformFamilyName(LandformFamily family) {
    const auto i=static_cast<std::size_t>(family);
    return i<kFamilies.size() ? kFamilies[i] : "unknown";
}

Fixed TerrainReference::sample(Fixed u, Fixed v) const {
    if (width<2 || height<2 || heights.size()!=std::size_t(width)*height) return offset;
    u=core::saturate(u)*Fixed::fromInt(width-1);
    v=core::saturate(v)*Fixed::fromInt(height-1);
    const auto x=std::min<std::uint32_t>(u.toInt(),width-2), y=std::min<std::uint32_t>(v.toInt(),height-2);
    const auto at=[&](int dx,int dy) { return Fixed::fromInt(heights[std::size_t(y+dy)*width+x+dx]); };
    return offset+scale*core::lerp(core::lerp(at(0,0),at(1,0),u-Fixed::fromInt(x)),
                                  core::lerp(at(0,1),at(1,1),u-Fixed::fromInt(x)),v-Fixed::fromInt(y));
}

Fixed TerrainReference::sampleMountain(Fixed u, Fixed v) const {
    if (width<2 || height<2 || spacing<=core::kZero) return sample(u,v);
    const auto du=Fixed::fromInt(96)/(spacing*static_cast<std::int64_t>(width-1));
    const auto dv=Fixed::fromInt(96)/(spacing*static_cast<std::int64_t>(height-1));
    constexpr int weights[5]={1,4,6,4,1};
    Fixed smoothed;
    for (int y=-2;y<=2;++y) for (int x=-2;x<=2;++x)
        smoothed+=sample(u+du*x,v+dv*y)*(weights[x+2]*weights[y+2]);
    smoothed=smoothed/256;
    return core::lerp(smoothed,sample(u,v),Fixed::ratio(3,20));
}

TerrainReference loadTerrainReference(const std::filesystem::path& metadata) {
    try {
        std::ifstream in(metadata);
        if (!in) throw std::runtime_error("cannot open metadata");
        json j; in >> j;
        TerrainReference r;
        r.id=j.at("id").get<std::string>(); r.family=familyOf(j.at("family").get<std::string>());
        const auto width=j.at("width").get<std::int64_t>(), height=j.at("height").get<std::int64_t>();
        if (width<2 || height<2 || width>4096 || height>4096 || width*height>4*1024*1024)
            throw std::runtime_error("invalid height dimensions");
        if (j.at("format")!="uint16_little_endian_row_major_no_header" ||
            j.at("row_order")!="north_to_south" || j.at("column_order")!="west_to_east" ||
            j.at("binary")!="height.h16") throw std::runtime_error("unsupported height encoding/orientation");
        const auto number=[&](const char* key,double lo,double hi) {
            const double v=j.at(key).get<double>();
            if (!std::isfinite(v) || v<lo || v>hi) throw std::runtime_error(std::string("invalid ")+key);
            return Fixed::fromDoubleForContent(v);
        };
        r.width=static_cast<std::uint32_t>(width); r.height=static_cast<std::uint32_t>(height);
        r.spacing=number("spacing_metres",0.01,1000);
        r.offset=number("height_offset_metres",-12000,12000);
        r.scale=number("height_scale_metres",0.000001,1);
        r.relief=r.scale*65535;
        std::ifstream binary(metadata.parent_path()/"height.h16",std::ios::binary|std::ios::ate);
        const auto bytes=static_cast<std::size_t>(width*height*2);
        if (!binary || binary.tellg()!=static_cast<std::streamoff>(bytes))
            throw std::runtime_error("height.h16 size mismatch");
        std::vector<unsigned char> raw(bytes);
        binary.seekg(0); binary.read(reinterpret_cast<char*>(raw.data()),static_cast<std::streamsize>(bytes));
        if (!binary) throw std::runtime_error("truncated height.h16");
        r.heights.resize(bytes/2);
        for (std::size_t i=0;i<r.heights.size();++i) r.heights[i]=raw[i*2]|(std::uint16_t(raw[i*2+1])<<8);
        core::Checksum hash;
        hash.add(core::hashBytes(raw.data(),raw.size())); hash.add(r.id); hash.add(static_cast<int>(r.family));
        hash.add(r.offset.raw); hash.add(r.scale.raw); hash.add(r.spacing.raw); hash.add(width); hash.add(height);
        r.fingerprint=hash.value();
        // Border mean removes the DEM's absolute terrestrial altitude. The
        // original physical relief is scaled explicitly, never normalised blindly.
        Fixed border;
        for (int i=0;i<32;++i) {
            const auto t=Fixed::ratio(i,31);
            border+=r.sample(t,core::kZero)+r.sample(t,core::kOne)+r.sample(core::kZero,t)+r.sample(core::kOne,t);
        }
        r.borderHeight=border/128;
        return r;
    } catch (const std::exception& e) { throw std::runtime_error(metadata.string()+": "+e.what()); }
}

std::filesystem::path defaultTerrainReferenceRoot() {
    if (const char* p=std::getenv("ASR_TERRAIN_REFERENCE_DIR"); p && *p) return p;
    for (const auto& parent : {std::filesystem::path("."),std::filesystem::path(".."),std::filesystem::path("../..")}) {
        const auto root=std::filesystem::absolute(parent/"assets/generated/terrain_references").lexically_normal();
        if (std::filesystem::is_regular_file(root/"index.json")) return root;
    }
#ifdef ASR_SOURCE_DIR
    return std::filesystem::path(ASR_SOURCE_DIR)/"assets/generated/terrain_references";
#else
    return "assets/generated/terrain_references";
#endif
}

Fixed HybridTerrain::incisionAt(Fixed x,Fixed y) const {
    if (incision.empty() || incisionStep<=0 || x<core::kZero || y<core::kZero ||
        x>=Fixed::fromInt(std::int64_t(width)*kMetresPerCell) ||
        y>=Fixed::fromInt(std::int64_t(height)*kMetresPerCell)) return {};
    const auto columns=(std::int64_t(width)*kMetresPerCell+incisionStep-1)/incisionStep+1;
    const auto u=x/Fixed::fromInt(incisionStep),v=y/Fixed::fromInt(incisionStep);
    const auto ix=u.toInt(),iy=v.toInt();
    const auto at=[&](int dx,int dy) { return Fixed::ratio(incision[(iy+dy)*columns+ix+dx],10); };
    return core::lerp(core::lerp(at(0,0),at(1,0),u-Fixed::fromInt(ix)),
                      core::lerp(at(0,1),at(1,1),u-Fixed::fromInt(ix)),v-Fixed::fromInt(iy));
}

HybridTerrain::Sample HybridTerrain::sample(Fixed x, Fixed y) const {
    Sample result;
    const auto cx=floorDiv(x.toInt(),kMetresPerCell), cy=floorDiv(y.toInt(),kMetresPerCell);
    if (cx<0 || cy<0 || cx>=width || cy>=height || spatial_.empty()) return result;
    for (const auto index : spatial_[static_cast<std::size_t>(cy)*width+cx]) {
        const auto& p=patches[index];
        const auto weight=support(p,x,y);
        result.delta+=patchHeight(p,x,y);
        result.influence+=weight;
        if (p.family==LandformFamily::Volcanic) {
            const auto cover=version>=3 ? volcanicDeposits(p,x,y) : weight;
            result.volcanic+=cover;
            result.barren+=(version>=3 ? cover : weight*weight)*Fixed::ratio(p.freshness,255);
        }
    }
    result.influence=core::saturate(result.influence);
    result.volcanic=core::saturate(result.volcanic);
    result.barren=core::saturate(result.barren);
    const auto cut=incisionAt(x,y);
    result.delta-=cut;
    result.influence=std::max(result.influence,fade(cut/Fixed::fromInt(20)));
    return result;
}

Fixed HybridTerrain::residual(Fixed x, Fixed y, Fixed delta) const {
    if (macroDelta.size()!=static_cast<std::size_t>(width)*height) return {};
    const auto u=x/Fixed::fromInt(kMetresPerCell), v=y/Fixed::fromInt(kMetresPerCell);
    const auto ix=u.toInt(), iy=v.toInt();
    if (ix<0 || iy<0 || ix>=width || iy>=height) return {};
    const auto at=[&](int dx,int dy) {
        const auto cx=std::clamp<std::int64_t>(ix+dx,0,width-1), cy=std::clamp<std::int64_t>(iy+dy,0,height-1);
        return Fixed::fromInt(macroDelta[static_cast<std::size_t>(cy)*width+cx]);
    };
    Fixed rows[4];
    for (int j=0;j<4;++j) rows[j]=spline(at(-1,j-1),at(0,j-1),at(1,j-1),at(2,j-1),u-Fixed::fromInt(ix));
    const auto coarse=spline(rows[0],rows[1],rows[2],rows[3],v-Fixed::fromInt(iy));
    // The macro component already shaped drainage. Add ONLY its missing detail;
    // never stamp a second mountain on top of the first one. Fixed envelope is
    // shared with height quantisation/culling through HeightField's margin.
    return core::clamp(delta-coarse,Fixed::fromInt(-kMaxResidualMetres),Fixed::fromInt(kMaxResidualMetres));
}

void HybridTerrain::prepare() {
    // Sixteen million cells, which covers the largest preset there is. It was
    // four million - a guard written when the biggest world was a quarter of
    // this one - and a giant world died on it after two gigabytes of macro map
    // had already been built. What this actually holds is one sixteen-bit delta
    // per cell: thirty megabytes at the new limit, which is not the thing worth
    // guarding against.
    if (width<=0 || height<=0 || std::int64_t(width)*height>16*1024*1024 || patches.size()>64 ||
        macroDelta.size()!=static_cast<std::size_t>(width)*height) throw std::runtime_error("invalid hybrid terrain dimensions");
    spatial_.assign(static_cast<std::size_t>(width)*height,{});
    if (version<1 || version>kVersion) throw std::runtime_error("invalid hybrid version");
    core::Checksum hash; hash.add(version); hash.add(width); hash.add(height);
    for (const auto d : macroDelta) {
        if (d < -2400 || d > 2400) throw std::runtime_error("invalid hybrid macro delta");
        hash.add(d);
    }
    for (std::size_t i=0;i<patches.size();++i) {
        const auto& p=patches[i];
        if (p.radius<1024 || p.radius>kMaxPatchRadiusMetres || p.radius%kSampleMetres ||
            p.side!=static_cast<std::uint32_t>(2*p.radius/kSampleMetres+1) ||
            p.delta.size()!=std::size_t(p.side)*p.side || p.family>=LandformFamily::Count ||
            p.originX<0 || p.originY<0 || std::int64_t(p.originX)+2*p.radius>=std::int64_t(width)*kMetresPerCell ||
            std::int64_t(p.originY)+2*p.radius>=std::int64_t(height)*kMetresPerCell)
            throw std::runtime_error("invalid hybrid patch");
        hash.add(p.referenceId); hash.add(p.sourceFingerprint); hash.add(static_cast<int>(p.family));
        hash.add(p.originX); hash.add(p.originY); hash.add(p.radius); hash.add(static_cast<int>(p.freshness)); hash.add(int(p.island));
        for (auto d : p.delta) hash.add(static_cast<int>(d));
        const int x0=p.originX/kMetresPerCell, y0=p.originY/kMetresPerCell;
        const int x1=(p.originX+2*p.radius)/kMetresPerCell, y1=(p.originY+2*p.radius)/kMetresPerCell;
        for (int y=y0;y<=y1;++y) for (int x=x0;x<=x1;++x)
            spatial_[static_cast<std::size_t>(y)*width+x].push_back(static_cast<std::uint32_t>(i));
    }
    if (version>=2) {
        if (incisionStep!=0 && (incisionStep<128 || incisionStep>1048576 || (incisionStep&(incisionStep-1))))
            throw std::runtime_error("invalid incision spacing");
        const auto columns=incisionStep ? (std::int64_t(width)*kMetresPerCell+incisionStep-1)/incisionStep+1 : 0;
        const auto rows=incisionStep ? (std::int64_t(height)*kMetresPerCell+incisionStep-1)/incisionStep+1 : 0;
        if (columns*rows>4*1024*1024 || incision.size()!=static_cast<std::size_t>(columns*rows))
            throw std::runtime_error("invalid incision grid size");
        hash.add(incisionStep);
        for (auto d : incision) {
            if (d>3000) throw std::runtime_error("invalid incision depth");
            hash.add(static_cast<int>(d));
        }
    } else if (incisionStep || !incision.empty()) throw std::runtime_error("legacy terrain has incisions");
    fingerprint_=hash.value();
}

static std::shared_ptr<HybridTerrain> buildHybridTerrain(WorldMapData& world, const WorldMapParams& params) {
    auto terrain=std::make_shared<HybridTerrain>();
    terrain->width=world.width; terrain->height=world.height;
    terrain->macroDelta.assign(world.cells.size(),0);
    // New worlds never load DEMs or synthesize separate mountain patches.
    // The existing payload reader remains available for historical saves.
    const std::vector<CatalogEntry> entries;
    updateCoast(world);
    bool branchedMassifPlaced=false;
    const auto bake=[&](int cx,int cy,LandformFamily family,bool island) {
        const auto seed=atSeed(world.seed^0x48594252,cx,cy);
        const bool branched=family==LandformFamily::Alpine && seed%5==0 && !branchedMassifPlaced;
        const bool mountain=family==LandformFamily::Alpine || family==LandformFamily::Glacial ||
                            family==LandformFamily::Folded || family==LandformFamily::Arid || family==LandformFamily::Old;
        LandformPatch p;
        // Leave room for extended daughters before the smooth perimeter fade.
        // Volcanoes/islands keep their complete original footprint.
        static_assert(mountains::kLocalSupportMetres+24*HybridTerrain::kSampleMetres<=
                      HybridTerrain::kMaxPatchRadiusMetres);
        const int mountainRadius=family==LandformFamily::Old ? 6144 :
            family==LandformFamily::Folded ? 8192 :
            branched ? mountains::kLocalSupportMetres : 7168;
        p.radius=(mountain ? mountainRadius : 3072)+
                 static_cast<int>((seed>>8)%25)*HybridTerrain::kSampleMetres;
        p.originX=cx*kMetresPerCell-p.radius; p.originY=cy*kMetresPerCell-p.radius;
        p.side=static_cast<std::uint32_t>(2*p.radius/HybridTerrain::kSampleMetres+1);
        p.family=family; p.island=island;
        p.freshness=family==LandformFamily::Volcanic ? static_cast<std::uint8_t>(128+(seed>>16)%128) : 0;
        if (p.originX<kMetresPerCell*2 || p.originY<kMetresPerCell*2 ||
            p.originX+2*p.radius>(world.width-3)*kMetresPerCell ||
            p.originY+2*p.radius>(world.height-3)*kMetresPerCell) return false;
        if (island) {
            const int reach=(p.radius+kMetresPerCell-1)/kMetresPerCell+1;
            for (int y=cy-reach;y<=cy+reach;++y) for (int x=cx-reach;x<=cx+reach;++x)
                if (!world.inBounds({x,y}) || !world.at({x,y}).sea) return false;
        }
        for (const auto& other : terrain->patches) {
            const auto dx=std::int64_t(p.originX+p.radius)-other.originX-other.radius;
            const auto dy=std::int64_t(p.originY+p.radius)-other.originY-other.radius;
            const auto reach=std::int64_t(p.radius)+other.radius+540;
            if (dx*dx+dy*dy<reach*reach) return false;
        }
        std::vector<const CatalogEntry*> choices;
        for (const auto& e : entries) if (e.family==family) choices.push_back(&e);
        std::shared_ptr<const TerrainReference> dem;
        for (std::size_t attempt=0;attempt<choices.size() && !dem;++attempt) {
            try { dem=reference(*choices[(seed%choices.size()+attempt)%choices.size()]); }
            catch (const std::exception& e) { terrain->diagnostics.push_back(e.what()); }
        }
        if (dem) { p.referenceId=dem->id; p.sourceFingerprint=dem->fingerprint; }
        else if (!entries.empty()) terrain->diagnostics.push_back(std::string("analytic fallback: ")+landformFamilyName(family));
        const Fixed centreX=Fixed::fromInt(cx*kMetresPerCell), centreY=Fixed::fromInt(cy*kMetresPerCell);
        const auto centre=baseAt(world,centreX,centreY);
        // Align to the regional contour using rational unit directions. There
        // is no platform-dependent sin/cos in placement or sampling.
        constexpr int directions[8][2]={{5,0},{4,3},{0,5},{-3,4},{-5,0},{-4,-3},{0,-5},{3,-4}};
        const auto gx=baseAt(world,centreX+Fixed::fromInt(540),centreY)-baseAt(world,centreX-Fixed::fromInt(540),centreY);
        const auto gy=baseAt(world,centreX,centreY+Fixed::fromInt(540))-baseAt(world,centreX,centreY-Fixed::fromInt(540));
        int direction=static_cast<int>(seed%8); Fixed best=Fixed::fromInt(-100000);
        if (family==LandformFamily::Folded || family==LandformFamily::Alpine || family==LandformFamily::Glacial) {
            for (int d=0;d<8;++d) {
                const auto score=-gy*directions[d][0]+gx*directions[d][1];
                if (score>best) { best=score; direction=d; }
            }
        }
        const auto a=Fixed::ratio(directions[direction][0],5), b=Fixed::ratio(directions[direction][1],5);
        const auto relief=Fixed::fromInt(family==LandformFamily::Old ? 400 : family==LandformFamily::Volcanic ? 2200 : 900);
        const Fixed vertical=dem ? std::min(Fixed::ratio(4,5),relief/std::max(dem->relief,core::kOne)) : core::kOne;
        p.delta.reserve(std::size_t(p.side)*p.side);
        for (std::uint32_t y=0;y<p.side;++y) for (std::uint32_t x=0;x<p.side;++x) {
            const auto wx=Fixed::fromInt(p.originX+x*HybridTerrain::kSampleMetres);
            const auto wy=Fixed::fromInt(p.originY+y*HybridTerrain::kSampleMetres);
            const auto mask=support(p,wx,wy);
            if (mask<=core::kZero) { p.delta.push_back(0); continue; }
            const auto dx=wx-centreX, dy=wy-centreY;
            auto rx=a*dx+b*dy, ry=-b*dx+a*dy;
            rx+=noise(seed^13,wx,wy,1400)*Fixed::fromInt(100);
            ry+=noise(seed^29,wx,wy,1400)*Fixed::fromInt(100);
            const auto base=baseAt(world,wx,wy);
            const auto r2=(dx*dx+dy*dy)/Fixed::fromInt(std::int64_t(p.radius)*p.radius);
            const auto broad=mountain || family==LandformFamily::Volcanic ? core::kZero : fractal(seed,rx,ry,family,false);
            const auto body=mountain ? massif(seed,rx,ry,p.radius,family,branched)*relief : core::kZero;
            Fixed shape;
            if (dem) {
                // The output grid is 32 m: low-pass four source samples rather
                // than pretending the 8 m DEM supports unlimited frequencies.
                const auto extentX=dem->spacing*static_cast<std::int64_t>(dem->width-1);
                const auto extentY=dem->spacing*static_cast<std::int64_t>(dem->height-1);
                const auto u=Fixed::ratio(1,2)+rx/(Fixed::fromInt(p.radius)*2)*Fixed::ratio(9,10);
                const auto v=Fixed::ratio(1,2)+ry/(Fixed::fromInt(p.radius)*2)*Fixed::ratio(9,10);
                const auto du=Fixed::fromInt(8)/extentX, dv=Fixed::fromInt(8)/extentY;
                const auto sampled=mountain ? dem->sampleMountain(u,v) :
                    (dem->sample(u-du,v-dv)+dem->sample(u+du,v-dv)+
                     dem->sample(u-du,v+dv)+dem->sample(u+du,v+dv))/4;
                shape=(sampled-dem->borderHeight)*vertical;
            } else if (mountain) shape=body;
            else if (family==LandformFamily::Volcanic) shape=noise(seed,rx,ry,1600)*Fixed::fromInt(60);
            else shape=broad*relief/2;
            if (dem && mountain) {
                // DEM keeps its natural drainage and identity; the shared body
                // supports its slopes instead of adding a second ridge graph.
                shape=core::lerp(body,shape,Fixed::ratio(3,4));
            }
            if (mountain && shape>core::kZero)
                shape*=core::saturate((Fixed::fromInt(2200)-base)/(relief*Fixed::ratio(6,5)));
            Fixed target=(mountain ? base : centre)+shape;
            if (family==LandformFamily::Volcanic) {
                // A complete edifice, not just a cropped summit: submarine
                // apron -> cone -> caldera. DEM supplies asymmetric flanks.
                const auto r=core::sqrt(r2);
                const auto rim=Fixed::ratio(18,100);
                // Truncate the summit first: subtracting a smooth bowl from a
                // pointed cone leaves a needle in the crater's very centre.
                const auto cone=core::saturate(core::kOne-std::max(r,rim));
                const auto crater=fade((rim-r)/rim);
                // Terraced, because a cone is BUILT and not poured.
                //
                // Each flow runs until it cools and stops where it stopped, so
                // a flank is a stack of low benches with short risers between
                // them - which is most of what tells the eye it is looking at a
                // volcano rather than at a smooth hill of the same shape. The
                // smooth profile is quantised into benches about eighty metres
                // apart, with a tread that is nearly level and a riser that is
                // short and eased, so nothing here is a cliff.
                //
                // The height is nudged by noise BEFORE it is quantised rather
                // than after. That moves where each bench breaks instead of
                // moving the bench itself, so the steps come out as lobes that
                // wander round the mountain - and not as the perfect concentric
                // rings a contour map is drawn with.
                const auto bench=relief/Fixed::fromInt(26);
                const auto wobble=noise(seed^0x7E44,rx,ry,260)*bench*Fixed::ratio(45,100);
                const auto stacked=core::max(core::kZero,cone*cone*relief+wobble)/bench;
                const auto whole=Fixed::fromInt(stacked.toInt());
                const auto within=stacked-whole;
                const auto tread=Fixed::ratio(35,100),riser=Fixed::ratio(30,100);
                const auto shaped=within<tread ? core::kZero
                                 :within>tread+riser ? core::kOne
                                 :fade((within-tread)/riser);
                const auto edifice=(whole+shaped)*bench-crater*relief/4;
                const auto deposits=volcanicDeposits(p,wx,wy)*relief/12;
                // Add to the local bed instead of replacing an entire hillside
                // with the centre's plane. The broad apron follows its terrain.
                // Signed flank detail must not excavate the pre-volcanic bed
                // where the cone and deposits fade out. Bound the added relief,
                // not the absolute height, so high local ground stays intact.
                const auto added=std::max(core::kZero,edifice+deposits+shape*(core::kOne-crater)/3);
                if (island) {
                    // An island is what the volcano BUILT, not the part of it
                    // that happens to clear the water.
                    //
                    // The bed here was a flat hundred metres down, so the only
                    // land was wherever the cone itself rose past it - a cone
                    // standing in the sea, with the waterline cutting across its
                    // flank at whatever height the arithmetic put it. Real ones
                    // spend their lives piling their own flanks into a shield,
                    // and what you land on is that shield with the cone in the
                    // middle of it.
                    //
                    // A square root, because a shield is steep nowhere and its
                    // slope falls off outward: three hundred metres under the
                    // summit, eighty at four fifths of the way out, and through
                    // sea level at about nine tenths - so the outer third of the
                    // island is low ground rather than more mountain.
                    //
                    // And the shore is not a circle. The radius is pushed around
                    // by two octaves of noise before the water is worked out, at
                    // the scale of the island and at a fraction of it, which is
                    // the difference between a coastline and a compass arc.
                    const auto wobble=noise(seed^0x1517,wx,wy,1500)*Fixed::ratio(22,100)+
                                      noise(seed^0x1518,wx,wy,520)*Fixed::ratio(9,100);
                    const auto shore=core::saturate(core::kOne-r*(core::kOne+wobble));
                    target=core::sqrt(shore)*Fixed::fromInt(420)-Fixed::fromInt(110)+added;
                } else {
                    target=base+added;
                }
            } else if (!dem) {
                if (family==LandformFamily::Canyon) target=centre-core::abs(broad)*relief/2;
                if (family==LandformFamily::Towers) target=centre+core::saturate(broad)*core::saturate(broad)*relief;
            }
            // Context-fitting plane preserves the regional direction. Fine
            // fractals decorate the DEM without duplicating its mountain scale.
            if (!island && !mountain && family!=LandformFamily::Volcanic)
                target+=(gx*dx+gy*dy)/Fixed::fromInt(1080)*Fixed::ratio(1,4);
            if (mountain) {
                const auto high=core::saturate(shape/relief);
                // No uniform layer of new summits: at most two metres near a
                // high crest, tapering to nothing on its foot and valley floor.
                target+=noise(seed^0xf12a,rx,ry,512)*Fixed::fromInt(2)*high*high;
            } else if (family!=LandformFamily::Volcanic) {
                const auto fine=fractal(seed^0xf12a,rx,ry,family,true)*Fixed::fromInt(12);
                target+=fine*(Fixed::ratio(1,2)+core::abs(broad)/2);
            }
            if (family!=LandformFamily::Volcanic)
                target=core::clamp(target,Fixed::fromInt(island ? -100 : 9),Fixed::fromInt(2200));
            const auto delta=(target-base)*mask;
            p.delta.push_back(static_cast<std::int16_t>(std::clamp<std::int64_t>((delta*10).roundToInt(),-30000,30000)));
        }
        terrain->patches.push_back(std::move(p));
        branchedMassifPlaced|=branched;
        return true;
    };

    // Volcanoes first, ranked by tectonic evidence; isolated oceanic candidates
    // also represent hotspots. A sea footprint must fit wholly off the mainland.
    for (bool island : {true,false}) {
        std::vector<std::pair<std::int64_t,std::size_t>> candidates;
        for (int y=10;y<world.height-10;++y) for (int x=10;x<world.width-10;++x) {
            const auto i=std::size_t(y)*world.width+x;
            if (world.cells[i].sea!=island || world.distanceToCoast[i]<(island ? 9 : 5)) continue;
            if (!island && world.cells[i].elevation>150) continue;
            const auto h=atSeed(world.seed^0x701ca,x,y);
            const auto score=std::int64_t(world.upliftField[i])*4+world.riftField[i]*3+(h%500);
            candidates.emplace_back(score,i);
        }
        std::sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b) {
            return a.first!=b.first ? a.first>b.first : a.second<b.second;
        });
        const int wanted=std::clamp(world.width*world.height/12000,1,3);
        int placed=0;
        for (const auto& candidate : candidates) {
            if (bake(static_cast<int>(candidate.second%world.width),static_cast<int>(candidate.second/world.width),
                     LandformFamily::Volcanic,island) && ++placed>=wanted) break;
        }
    }
    terrain->prepare(); // slope erosion now belongs to the common H64 pipeline
    return terrain;
}

void generateHybridTerrain(WorldMapData& world, const WorldMapParams& params) {
    world.hybridTerrain.reset();
    if (!params.hybridTerrain || world.width<24 || world.height<24) return;
    auto terrain = params.hybridSnapshot ? std::make_shared<HybridTerrain>(*params.hybridSnapshot) :
                                          buildHybridTerrain(world,params);
    if (terrain->width!=world.width || terrain->height!=world.height)
        throw std::runtime_error("hybrid snapshot/world dimensions differ");
    // Reconstruct the same macro contribution, then let downstream passes
    // recompute climate/drainage from it exactly as on the first generation.
    terrain->macroDelta.assign(world.cells.size(),0);
    terrain->prepare();
    for (int y=0;y<world.height;++y) for (int x=0;x<world.width;++x) {
        const auto i=std::size_t(y)*world.width+x;
        const auto sample=terrain->sample(Fixed::fromInt(x*kMetresPerCell),Fixed::fromInt(y*kMetresPerCell));
        auto& c=world.cells[i]; const auto before=cellHeight(c);
        const auto after=before+sample.delta;
        if (sample.influence>core::kZero) {
            c.sea=after<=core::kZero;
            c.elevation=c.sea ? 0 : static_cast<std::uint8_t>(std::clamp<std::int64_t>(
                (after/Fixed::fromInt(kMetresPerElevationStep)).roundToInt(),1,255));
            terrain->macroDelta[i]=static_cast<std::int32_t>((cellHeight(c)-before).toInt());
        }
        if (sample.volcanic>Fixed::ratio(1,10)) {
            world.rockTypeField[i]=RockType::Volcanic;
            world.soilParentMaterialField[i]=3;
            world.erosionResistanceField[i]=210;
            world.permeabilityField[i]=150;
        }
    }
    updateCoast(world);
    terrain->prepare();
    world.hybridTerrain=std::move(terrain);
}

std::string saveHybridTerrain(const HybridTerrain& terrain) {
    json j={{"version",terrain.version},{"width",terrain.width},{"height",terrain.height},
            {"fingerprint",terrain.fingerprint()},{"macro_delta",terrain.macroDelta},{"patches",json::array()}};
    if (terrain.version>=2) { j["incision_step"]=terrain.incisionStep; j["incision_dm"]=terrain.incision; }
    for (const auto& p : terrain.patches) j["patches"].push_back({
        {"reference",p.referenceId},{"source_fingerprint",p.sourceFingerprint},{"family",landformFamilyName(p.family)},
        {"x",p.originX},{"y",p.originY},{"radius",p.radius},{"side",p.side},{"freshness",p.freshness},
        {"island",p.island},{"delta_dm",p.delta}});
    return j.dump();
}

std::shared_ptr<const HybridTerrain> loadHybridTerrain(const std::string& payload,int width,int height) {
    const auto j=json::parse(payload);
    const auto version=j.at("version").get<int>();
    if (version<1 || version>HybridTerrain::kVersion || j.at("width")!=width || j.at("height")!=height ||
        j.at("patches").size()>64 || width<=0 || height<=0 || std::int64_t(width)*height>4*1024*1024 ||
        j.at("macro_delta").size()!=static_cast<std::size_t>(width)*height)
        throw std::runtime_error("incompatible hybrid terrain save");
    auto terrain=std::make_shared<HybridTerrain>();
    terrain->width=width; terrain->height=height;
    terrain->version=version;
    terrain->macroDelta=j.at("macro_delta").get<std::vector<std::int32_t>>();
    if (version>=2) {
        terrain->incisionStep=j.at("incision_step").get<int>();
        if (j.at("incision_dm").size()>4*1024*1024) throw std::runtime_error("oversized incision grid");
        for (const auto& d : j.at("incision_dm")) {
            const auto value=d.get<std::int64_t>();
            if (value<0 || value>3000) throw std::runtime_error("invalid saved incision depth");
            terrain->incision.push_back(static_cast<std::uint16_t>(value));
        }
    }
    for (const auto& v : j.at("patches")) {
        LandformPatch p;
        p.referenceId=v.at("reference").get<std::string>(); p.sourceFingerprint=v.at("source_fingerprint").get<std::uint64_t>();
        p.family=familyOf(v.at("family").get<std::string>());
        p.originX=v.at("x").get<int>(); p.originY=v.at("y").get<int>(); p.radius=v.at("radius").get<int>();
        const auto side=v.at("side").get<std::int64_t>(); const auto freshness=v.at("freshness").get<int>();
        if (side<2 || side>HybridTerrain::kMaxPatchSide || freshness<0 || freshness>255 ||
            v.at("delta_dm").size()!=std::size_t(side)*side)
            throw std::runtime_error("invalid hybrid patch size");
        p.side=static_cast<std::uint32_t>(side); p.freshness=static_cast<std::uint8_t>(freshness);
        p.island=v.at("island").get<bool>();
        for (const auto& d : v.at("delta_dm")) {
            const auto value=d.get<std::int64_t>();
            if (value < -30000 || value > 30000) throw std::runtime_error("invalid hybrid height");
            p.delta.push_back(static_cast<std::int16_t>(value));
        }
        terrain->patches.push_back(std::move(p));
    }
    terrain->prepare();
    if (terrain->fingerprint()!=j.at("fingerprint").get<std::uint64_t>())
        throw std::runtime_error("hybrid terrain checksum mismatch");
    return terrain;
}
} // namespace generation
