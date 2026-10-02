#include "framework.hpp"
#include "game/generation/hybrid_terrain.hpp"
#include "game/generation/mountain_shape.hpp"
#include "game/generation/world_map_gen.hpp"
#include "game/world/height_field.hpp"
#include "game/world/ring_mesh.hpp"
#include "game/world/terrain_streaming/base_tile_baker.hpp"
#include "game/world/terrain_streaming/hydrology_builder.hpp"
#include "game/world/terrain_adaptive.hpp"
#include "game/world/terrain_streaming/page_store.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <set>
#include <thread>

namespace {
using core::Fixed;
using namespace generation;
using nlohmann::json;

struct Directory {
    std::filesystem::path path;
    Directory() {
        static std::atomic<unsigned> sequence{0};
        path=std::filesystem::temp_directory_path()/("asr-hybrid-"+
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(sequence++));
        std::filesystem::create_directories(path);
    }
    ~Directory() { std::error_code ignored; std::filesystem::remove_all(path,ignored); }
};

void tinyReference(const std::filesystem::path& root) {
    std::filesystem::create_directories(root/"test");
    json metadata={{"id","test"},{"family","volcanic"},{"width",2},{"height",2},
        {"spacing_metres",8184},{"height_offset_metres",100},{"height_scale_metres",0.01},
        {"format","uint16_little_endian_row_major_no_header"},{"row_order","north_to_south"},
        {"column_order","west_to_east"},{"binary","height.h16"}};
    // Use a supported source spacing; this miniature fixture tests encoding,
    // not the metric extent of a real 1024-square reference.
    metadata["spacing_metres"]=8;
    std::ofstream(root/"test/metadata.json")<<metadata;
    std::ofstream out(root/"test/height.h16",std::ios::binary);
    for (unsigned v : {0u,10000u,30000u,65535u}) {
        out.put(static_cast<char>(v&255)); out.put(static_cast<char>(v>>8));
    }
    std::ofstream(root/"index.json")<<json{{"regions",json::array({{{"id","test"},{"family","volcanic"}}})}};
}

WorldMapData simpleCountry(int side=64) {
    WorldMapData w; w.width=w.height=side; w.seed=42;
    const auto n=std::size_t(w.width)*w.height;
    w.cells.resize(n); w.upliftField.assign(n,0); w.riftField.assign(n,0); w.faultField.assign(n,0);
    w.rockTypeField.assign(n,RockType::HardRock); w.soilParentMaterialField.assign(n,0);
    w.erosionResistanceField.assign(n,200); w.permeabilityField.assign(n,100);
    for (int y=0;y<w.height;++y) for (int x=0;x<w.width;++x) {
        auto& c=w.at({x,y}); c.sea=x<32; c.elevation=c.sea ? 0 : 65;
        c.moisture=140; c.temperature=150; c.fertility=90;
        c.climate=Climate::TemperateForest;
    }
    return w;
}
WorldMapParams analyticParams() {
    WorldMapParams p; p.width=p.height=64; p.seed=42;
    p.terrainReferenceRoot=std::filesystem::temp_directory_path()/"asr-intentionally-absent-dem-library";
    return p;
}
const WorldMapData& generated() {
    // A 64-cell country may have no land footprint wide enough for a complete
    // massif; use a larger map so perimeter/save tests actually exercise one.
    static const auto w=[] {
        auto p=analyticParams(); p.width=p.height=128;
        return generateWorldMap(p);
    }();
    return w;
}
} // namespace

TEST(hybrid_dem_decodes_little_endian_metres_and_bilinear_samples) {
    Directory dir; tinyReference(dir.path);
    const auto r=loadTerrainReference(dir.path/"test/metadata.json");
    CHECK_EQ(r.heights[3],std::uint16_t(65535));
    CHECK_EQ(r.sample(core::kZero,core::kZero),Fixed::fromInt(100));
    CHECK(core::abs(r.sample(core::kOne,core::kZero)-Fixed::fromInt(200))<Fixed::ratio(1,1000));
    CHECK(core::abs(r.sample(core::kZero,core::kOne)-Fixed::fromInt(400))<Fixed::ratio(1,1000));
    const auto expected=Fixed::fromInt(100)+r.scale*Fixed::ratio(105535,4);
    CHECK_EQ(r.sample(Fixed::ratio(1,2),Fixed::ratio(1,2)),expected);
    CHECK_EQ(r.sample(Fixed::fromInt(-2),Fixed::fromInt(3)),r.sample(core::kZero,core::kOne));
    CHECK(r.fingerprint!=0);
}

TEST(hybrid_dem_rejects_truncation_and_invalid_metadata) {
    Directory dir; tinyReference(dir.path);
    const auto metadata=dir.path/"test/metadata.json";
    std::ofstream(dir.path/"test/height.h16",std::ios::binary|std::ios::trunc).put(0);
    bool rejected=false;
    try { (void)loadTerrainReference(metadata); } catch (const std::exception&) { rejected=true; }
    CHECK(rejected);
    tinyReference(dir.path);
    json j; std::ifstream(metadata)>>j; j["height_scale_metres"]=-1;
    std::ofstream(metadata)<<j;
    rejected=false;
    try { (void)loadTerrainReference(metadata); } catch (const std::exception&) { rejected=true; }
    CHECK(rejected);
}

TEST(hybrid_places_complete_procedural_volcanic_islands_without_dem_content) {
    Directory dir; tinyReference(dir.path);
    auto w=simpleCountry(); auto p=analyticParams(); p.terrainReferenceRoot=dir.path;
    generateHybridTerrain(w,p);
    CHECK(w.hybridTerrain!=nullptr);
    if (!w.hybridTerrain) return;
    bool island=false,dem=false,volcanicLand=false;
    for (const auto& patch : w.hybridTerrain->patches) {
        dem|=patch.referenceId=="test";
        if (!patch.island) continue;
        island=true;
        const int x=(patch.originX+patch.radius)/kMetresPerCell, y=(patch.originY+patch.radius)/kMetresPerCell;
        CHECK(!w.at({x,y}).sea);
        const auto wx=Fixed::fromInt(x*kMetresPerCell), wy=Fixed::fromInt(y*kMetresPerCell);
        const auto centre=w.hybridTerrain->sample(wx,wy).delta;
        const auto rim=w.hybridTerrain->sample(wx+Fixed::ratio(patch.radius*18,100),wy).delta;
        CHECK(rim>centre+Fixed::fromInt(200)); // a caldera, not a cone with a needle inside
        CHECK(core::abs(w.hybridTerrain->residual(wx,wy,centre))<=Fixed::ratio(46,10));
        CHECK(w.at({x-9,y}).sea && w.at({x+9,y}).sea);
        CHECK(w.at({x,y-9}).sea && w.at({x,y+9}).sea);
    }
    for (std::size_t i=0;i<w.cells.size();++i)
        volcanicLand|=!w.cells[i].sea && w.rockTypeField[i]==RockType::Volcanic;
    CHECK(island && !dem && volcanicLand);
}

TEST(hybrid_fallback_is_deterministic_and_can_be_disabled) {
    auto a=simpleCountry(),b=a; auto p=analyticParams();
    generateHybridTerrain(a,p); generateHybridTerrain(b,p);
    CHECK(a.hybridTerrain && b.hybridTerrain);
    if (!a.hybridTerrain || !b.hybridTerrain) return;
    CHECK(!a.hybridTerrain->patches.empty());
    CHECK(a.hybridTerrain->diagnostics.empty()); // no attempted external library access
    CHECK_EQ(a.hybridTerrain->fingerprint(),b.hybridTerrain->fingerprint());
    CHECK_EQ(a.hybridTerrain->macroDelta,b.hybridTerrain->macroDelta);
    for (std::size_t i=0;i<a.cells.size();++i) {
        CHECK_EQ(a.cells[i].elevation,b.cells[i].elevation); CHECK_EQ(a.cells[i].sea,b.cells[i].sea);
    }
    auto legacy=simpleCountry(); const auto original=legacy.cells;
    p.hybridTerrain=false; generateHybridTerrain(legacy,p);
    CHECK(!legacy.hybridTerrain);
    for (std::size_t i=0;i<original.size();++i) CHECK_EQ(legacy.cells[i].elevation,original[i].elevation);
}

TEST(hybrid_volcanic_relief_follows_local_ground_and_deposits_are_not_circular) {
    auto flat=simpleCountry();
    for (auto& c:flat.cells) { c.sea=false; c.elevation=65; }
    auto hillside=flat;
    for (int y=0;y<hillside.height;++y) for (int x=0;x<hillside.width;++x)
        hillside.at({x,y}).elevation=std::uint8_t(45+x/2);
    const auto params=analyticParams();
    generateHybridTerrain(flat,params); generateHybridTerrain(hillside,params);
    CHECK(flat.hybridTerrain && hillside.hybridTerrain);
    if (!flat.hybridTerrain || !hillside.hybridTerrain) return;
    const auto& t=*flat.hybridTerrain;
    CHECK(!t.patches.empty()); CHECK_EQ(t.patches.size(),hillside.hybridTerrain->patches.size());
    const auto copy=loadHybridTerrain(saveHybridTerrain(t),flat.width,flat.height);
    auto legacy=t; legacy.version=2; legacy.prepare();
    const auto old=loadHybridTerrain(saveHybridTerrain(legacy),flat.width,flat.height);
    for (std::size_t i=0;i<t.patches.size();++i) {
        const auto& p=t.patches[i]; CHECK(!p.island);
        if (i>=hillside.hybridTerrain->patches.size()) return;
        const auto& q=hillside.hybridTerrain->patches[i];
        CHECK_EQ(p.originX,q.originX); CHECK_EQ(p.originY,q.originY);
        // Depositing on a slope must not dig away the uphill half or flatten
        // its foot to a centre plane. Only the original bed changes here.
        CHECK_EQ(p.delta,q.delta);
        CHECK(*std::min_element(p.delta.begin(),p.delta.end())>=0);
        CHECK(*std::max_element(p.delta.begin(),p.delta.end())>15000);
        const auto cx=Fixed::fromInt(p.originX+p.radius),cy=Fixed::fromInt(p.originY+p.radius);
        CHECK(t.sample(cx+Fixed::ratio(p.radius*18,100),cy).delta>t.sample(cx,cy).delta+Fixed::fromInt(400));
        Fixed low=core::kOne,high,legacyCover=Fixed::fromInt(-1);
        for (const auto& direction:std::array<std::array<int,2>,4>{{{1,0},{0,1},{-1,0},{0,-1}}}) {
            const auto x=cx+Fixed::ratio(p.radius*3*direction[0],4);
            const auto y=cy+Fixed::ratio(p.radius*3*direction[1],4);
            const auto s=t.sample(x,y),saved=copy->sample(x,y),v2=old->sample(x,y);
            low=std::min(low,s.volcanic); high=std::max(high,s.volcanic);
            CHECK_EQ(s.delta,saved.delta); CHECK_EQ(s.volcanic,saved.volcanic); CHECK_EQ(s.barren,saved.barren);
            CHECK(s.barren<=s.volcanic && s.barren>=core::kZero);
            if (legacyCover>=core::kZero) CHECK_EQ(v2.volcanic,legacyCover);
            legacyCover=v2.volcanic;
            const auto edge=t.sample(cx+Fixed::fromInt(p.radius*direction[0]),cy+Fixed::fromInt(p.radius*direction[1]));
            CHECK_EQ(edge.volcanic,core::kZero); CHECK_EQ(edge.barren,core::kZero);
        }
        CHECK(high>Fixed::ratio(1,10)); // deposits reach well beyond the caldera/cone core
        CHECK(high-low>Fixed::ratio(1,20)); // an irregular apron, not a painted disc
    }
}

TEST(hybrid_leaves_nonvolcanic_massifs_entirely_to_tectonics) {
    // Plenty of eligible mountain placements; a small island fixture could
    // pass a mere <=1 check without ever attempting a second branching form.
    auto w=simpleCountry(192);
    for (auto& c : w.cells) { c.sea=false; c.elevation=170; }
    auto params=analyticParams(); params.width=params.height=192;
    generateHybridTerrain(w,params);
    CHECK(w.hybridTerrain); if (!w.hybridTerrain) return;
    int alpine=0,branched=0,glacial=0;
    for (const auto& patch : w.hybridTerrain->patches) {
        glacial+=patch.family==LandformFamily::Glacial;
        if (patch.family!=LandformFamily::Alpine) continue;
        ++alpine;
        // Only the branching variant needs the expanded skeleton footprint.
        branched+=patch.radius>=mountains::kLocalSupportMetres;
    }
    CHECK_EQ(alpine,0); CHECK_EQ(glacial,0); CHECK_EQ(branched,0);
    for (const auto& patch:w.hybridTerrain->patches) CHECK_EQ(patch.family,LandformFamily::Volcanic);
}

TEST(hybrid_patch_perimeters_are_zero_and_continuous) {
    const auto& w=generated(); CHECK(w.hybridTerrain && !w.hybridTerrain->patches.empty());
    if (!w.hybridTerrain) return;
    bool extendedMountain=false;
    for (const auto& p : w.hybridTerrain->patches) {
        const bool mountain=p.family==LandformFamily::Alpine || p.family==LandformFamily::Glacial ||
            p.family==LandformFamily::Folded || p.family==LandformFamily::Arid || p.family==LandformFamily::Old;
        CHECK(p.radius>=(mountain ? 6144 : 3072));
        CHECK(p.radius<=(mountain ? mountains::kLocalSupportMetres+768 : 3840));
        extendedMountain|=mountain;
        for (std::uint32_t i=0;i<p.side;++i) {
            CHECK_EQ(p.delta[i],0); CHECK_EQ(p.delta[(p.side-1)*p.side+i],0);
            CHECK_EQ(p.delta[i*p.side],0); CHECK_EQ(p.delta[i*p.side+p.side-1],0);
        }
        const auto x=Fixed::fromInt(p.originX),y=Fixed::fromInt(p.originY+p.radius);
        const auto at=w.hybridTerrain->sample(x,y);
        CHECK_EQ(at.delta+w.hybridTerrain->incisionAt(x,y),core::kZero);
        const auto next=x+Fixed::fromInt(1);
        CHECK(core::abs(w.hybridTerrain->sample(next,y).delta+w.hybridTerrain->incisionAt(next,y))<Fixed::ratio(1,10));
        CHECK(core::abs(w.hybridTerrain->incisionAt(next,y)-w.hybridTerrain->incisionAt(x,y))<Fixed::fromInt(3));
    }
    CHECK(!extendedMountain);
    CHECK_EQ(w.hybridTerrain->sample(Fixed::fromInt(-1),Fixed::fromInt(-1)).delta,core::kZero);
}

TEST(hybrid_saved_world_is_self_contained_and_detects_corruption) {
    Directory dir;
    const auto& w=generated(); CHECK(w.hybridTerrain!=nullptr); if (!w.hybridTerrain) return;
    const auto payload=saveHybridTerrain(*w.hybridTerrain);
    const auto copy=loadHybridTerrain(payload,w.width,w.height);
    CHECK_EQ(copy->fingerprint(),w.hybridTerrain->fingerprint());
    CHECK_EQ(copy->patches.front().delta,w.hybridTerrain->patches.front().delta);
    bool largePatch=false;
    for (std::size_t i=0;i<copy->patches.size();++i) {
        const auto& p=copy->patches[i];
        largePatch|=p.radius>=6144; // family-sized massifs, not mandatory identical footprints
        CHECK_EQ(p.delta,w.hybridTerrain->patches[i].delta);
        CHECK_EQ(p.radius,w.hybridTerrain->patches[i].radius);
    }
    CHECK(!largePatch);
    auto corrupt=json::parse(payload); corrupt["patches"][0]["delta_dm"][10]=123;
    bool rejected=false;
    try { (void)loadHybridTerrain(corrupt.dump(),w.width,w.height); } catch (const std::exception&) { rejected=true; }
    CHECK(rejected);
    for (const auto* field : {"side","radius"}) {
        corrupt=json::parse(payload); corrupt["patches"][0][field]=1000000;
        rejected=false;
        try { (void)loadHybridTerrain(corrupt.dump(),w.width,w.height); } catch (const std::exception&) { rejected=true; }
        CHECK(rejected);
    }
    CHECK(saveWorldMapData(w,dir.path/"world.json"));
    WorldMapData loaded; CHECK(loadWorldMapData(dir.path/"world.json",loaded));
    CHECK(loaded.hybridTerrain!=nullptr); if (!loaded.hybridTerrain) return;
    CHECK_EQ(loaded.hybridTerrain->fingerprint(),w.hybridTerrain->fingerprint());
    world::HeightField a(&w,w.seed),b(&loaded,loaded.seed);
    for (const auto& p : w.hybridTerrain->patches) {
        const auto x=(p.originX+p.radius)/4,y=(p.originY+p.radius)/4;
        CHECK_EQ(a.sampleHeight(x,y),b.sampleHeight(x,y));
    }
    auto legacy=w; legacy.hybridTerrain.reset(); legacy.terrainFoundation.reset();
    CHECK(saveWorldMapData(legacy,dir.path/"legacy.json"));
    CHECK(loadWorldMapData(dir.path/"legacy.json",loaded)); CHECK(!loaded.hybridTerrain);
}

TEST(hybrid_spatial_index_and_save_cover_expanded_patch_extremities) {
    HybridTerrain terrain;
    terrain.width=terrain.height=64;
    terrain.macroDelta.assign(64*64,0);
    // Old snapshots and newly enlarged patches use the same format.
    for (int radius : {8192,HybridTerrain::kMaxPatchRadiusMetres}) {
        LandformPatch p;
        p.family=LandformFamily::Alpine;
        p.originX=p.originY=kMetresPerCell;
        p.radius=radius;
        p.side=2*radius/HybridTerrain::kSampleMetres+1;
        p.delta.assign(std::size_t(p.side)*p.side,0);
        const auto column=p.side-3,row=p.side/2;
        p.delta[std::size_t(row)*p.side+column]=100;
        const auto x=Fixed::fromInt(p.originX+column*HybridTerrain::kSampleMetres);
        const auto y=Fixed::fromInt(p.originY+row*HybridTerrain::kSampleMetres);
        terrain.patches={std::move(p)};
        terrain.prepare();
        CHECK_EQ(terrain.sample(x,y).delta,Fixed::fromInt(10));
        const auto copy=loadHybridTerrain(saveHybridTerrain(terrain),64,64);
        CHECK_EQ(copy->sample(x,y).delta,Fixed::fromInt(10));
        CHECK_EQ(copy->fingerprint(),terrain.fingerprint());
    }
}

TEST(hybrid_height_field_is_worker_and_lod_invariant_with_safe_bounds) {
    const auto& w=generated(); CHECK(w.hybridTerrain!=nullptr); if (!w.hybridTerrain) return;
    const auto q=world::streaming::hsimQuantisationFor(w);
    world::HeightField first(&w,w.seed),second(&w,w.seed);
    for (const auto& p : w.hybridTerrain->patches) {
        const auto sx=(p.originX+p.radius)/4,sy=(p.originY+p.radius)/4;
        for (int dy=-128;dy<=128;dy+=32) for (int dx=-128;dx<=128;dx+=32) {
            const auto h=first.sampleHeight(sx+dx,sy+dy);
            CHECK_EQ(h,second.sampleHeight(sx+dx,sy+dy));
            CHECK(h>=q.low && h<=q.high);
            for (int stride : {4,8,16,32,64,256}) CHECK_EQ(h,first.sampleHeight(sx+dx,sy+dy,stride));
        }
        CHECK_EQ(first.materialsGiven(sx,sy,core::kZero,core::kZero,1).weight,
                 second.materialsGiven(sx,sy,core::kZero,core::kZero,16).weight);
        const auto h=first.sampleHeight(sx,sy);
        Fixed elsewhere;
        std::thread worker([&] { world::HeightField local(&w,w.seed); elsewhere=local.sampleHeight(sx,sy); });
        worker.join(); CHECK_EQ(h,elsewhere);
    }
}

TEST(hybrid_baked_pages_share_border_heights) {
    using namespace world::streaming;
    const auto& w=generated(); if (!w.hybridTerrain || w.hybridTerrain->patches.empty()) return;
    const auto graph=buildHydrologyGraph(w);
    const BaseTileBaker baker(w,graph,hsimQuantisationFor(w));
    const auto& p=w.hybridTerrain->patches.front();
    const TileKey key{(p.originX+p.radius)/512,(p.originY+p.radius)/512,4};
    const auto a=baker.bake(key,64,1),b=baker.bake({key.x+1,key.y,4},64,1);
    CHECK(a.valid() && b.valid());
    const auto side=static_cast<std::size_t>(a.width+2*a.padding);
    for (std::size_t row=1;row+1<side;++row)
        CHECK_EQ(a.heightQuantized[row*side+a.padding+a.width-1],b.heightQuantized[row*side+b.padding]);
}

TEST(mountain_drainage_cuts_slopes_without_raising_peaks_or_reversing_flow) {
    constexpr int side=97,spacing=64;
    std::vector<Fixed> heights(side*side),limits(side*side,Fixed::fromInt(180));
    for (int y=0;y<side;++y) for (int x=0;x<side;++x) {
        const auto radius=core::hypot(Fixed::fromInt(x-side/2),Fixed::fromInt(y-side/2));
        heights[y*side+x]=Fixed::fromInt(1600)-radius*20;
        limits[y*side+x]*=mountains::smooth(radius/8);
    }
    const auto cuts=mountains::drainageIncisions(heights,limits,side,side,spacing);
    CHECK_EQ(cuts,mountains::drainageIncisions(heights,limits,side,side,spacing));
    CHECK_EQ(cuts[(side/2)*side+side/2],0);
    CHECK(*std::max_element(cuts.begin(),cuts.end())>100);
    CHECK(std::count_if(cuts.begin(),cuts.end(),[](auto c) { return c>10; })>side*side/5);
    for (int y=1;y<side-1;++y) for (int x=1;x<side-1;++x) {
        const auto i=y*side+x;
        int down=i; Fixed best;
        CHECK(Fixed::ratio(cuts[i],10)<=limits[i]);
        for (int dy=-1;dy<=1;++dy) for (int dx=-1;dx<=1;++dx) {
            if (!dx && !dy) continue;
            const auto j=(y+dy)*side+x+dx;
            const auto run=Fixed::fromInt(spacing)*(dx && dy ? Fixed::ratio(1414,1000) : core::kOne);
            const auto slope=(heights[i]-heights[j])/run;
            if (slope>best) { best=slope; down=j; }
        }
        if (down!=i) CHECK(heights[i]-Fixed::ratio(cuts[i],10)>=heights[down]-Fixed::ratio(cuts[down],10));
    }
    std::fill(heights.begin(),heights.end(),Fixed::fromInt(1200));
    const auto flat=mountains::drainageIncisions(heights,limits,side,side,spacing);
    CHECK(std::all_of(flat.begin(),flat.end(),[](auto c) { return c==0; }));
    std::fill(limits.begin(),limits.end(),core::kZero);
    for (std::size_t i=0;i<heights.size();++i) heights[i]=Fixed::fromInt(i);
    const auto protectedGround=mountains::drainageIncisions(heights,limits,side,side,spacing);
    CHECK(std::all_of(protectedGround.begin(),protectedGround.end(),[](auto c) { return c==0; }));
}

TEST(hybrid_incisions_roundtrip_validate_and_keep_legacy_snapshots_readable) {
    const auto& w=generated(); CHECK(w.hybridTerrain); if (!w.hybridTerrain) return;
    CHECK(w.hybridTerrain->incision.empty()); // new erosion is in H64, not a hybrid stamp
    auto t=*w.hybridTerrain;
    t.incisionStep=128;
    t.incision.assign(std::size_t((w.width*kMetresPerCell+127)/128+1)*((w.height*kMetresPerCell+127)/128+1),120);
    t.prepare();
    CHECK_EQ(t.version,HybridTerrain::kVersion);
    CHECK(std::any_of(t.incision.begin(),t.incision.end(),[](auto c) { return c>100; }));
    const auto copy=loadHybridTerrain(saveHybridTerrain(t),w.width,w.height);
    CHECK_EQ(copy->incision,t.incision); CHECK_EQ(copy->incisionStep,t.incisionStep);
    for (int y=1;y<w.height;y+=7) for (int x=1;x<w.width;x+=7) {
        const auto wx=Fixed::fromInt(x*kMetresPerCell+47),wy=Fixed::fromInt(y*kMetresPerCell+81);
        CHECK_EQ(copy->sample(wx,wy).delta,t.sample(wx,wy).delta);
    }
    for (int mode=0;mode<4;++mode) {
        auto corrupt=json::parse(saveHybridTerrain(t));
        if (mode==0) corrupt["incision_dm"][0]=-1;
        if (mode==1) corrupt["incision_dm"][0]=3001;
        if (mode==2) corrupt["incision_step"]=127;
        if (mode==3) corrupt["incision_dm"][0]=corrupt["incision_dm"][0].get<int>()==0 ? 1 : 0;
        bool rejected=false;
        try { (void)loadHybridTerrain(corrupt.dump(),w.width,w.height); } catch (const std::exception&) { rejected=true; }
        CHECK(rejected);
    }
    auto legacy=t; legacy.version=1; legacy.incisionStep=0; legacy.incision.clear(); legacy.prepare();
    const auto old=loadHybridTerrain(saveHybridTerrain(legacy),w.width,w.height);
    CHECK_EQ(old->version,1); CHECK(old->incision.empty()); CHECK_EQ(old->fingerprint(),legacy.fingerprint());
}

TEST(mountain_material_exposes_cliffs_not_flat_high_ground) {
    auto w=simpleCountry();
    for (auto& c : w.cells) { c.sea=false; c.elevation=170; c.temperature=255; c.moisture=180; }
    auto t=std::make_shared<HybridTerrain>(); t->width=w.width; t->height=w.height;
    t->macroDelta.assign(w.cells.size(),0); t->prepare(); w.hybridTerrain=t;
    world::HeightField flat(&w,w.seed);
    for (int y=20;y<40;y+=3) for (int x=20;x<40;x+=3) {
        const auto sx=x*kMetresPerCell/world::kSampleMetres,sy=y*kMetresPerCell/world::kSampleMetres;
        CHECK(flat.sampleHeight(sx,sy)>Fixed::fromInt(1100));
        CHECK(flat.sampleSlope(sx,sy)<Fixed::ratio(3,5));
        CHECK_EQ(flat.materialsGiven(sx,sy,core::kZero,core::kZero,1).of(world::Material::Rock),core::kZero);
    }
    for (int y=0;y<w.height;++y) for (int x=0;x<w.width;++x)
        w.at({x,y}).elevation=static_cast<std::uint8_t>(std::clamp(128+(x-32)*100,1,255));
    world::HeightField cliff(&w,w.seed); int exposed=0;
    for (int y=20;y<40;y+=3) for (int x=31*kMetresPerCell;x<33*kMetresPerCell;x+=32) {
        const auto sx=x/world::kSampleMetres,sy=y*kMetresPerCell/world::kSampleMetres;
        if (cliff.sampleSlope(sx,sy)<Fixed::ratio(5,4)) continue;
        ++exposed;
        CHECK(cliff.materialsGiven(sx,sy,core::kZero,core::kZero,1).of(world::Material::Rock)>Fixed::ratio(1,2));
    }
    CHECK(exposed>10);
}

TEST(mountain_shape_is_a_connected_bounded_fractal_skeleton) {
    CHECK_EQ(mountains::kBranchGenerations,5);
    for (std::uint64_t seed : {1,7,42}) {
        const auto s=mountains::makeSkeleton(seed);
        CHECK(s.nodeCount<=mountains::kMaxNodes && s.edgeCount<=mountains::kMaxEdges);
        CHECK_EQ(s.edgeCount+1,s.nodeCount); // a connected branching tree, not scattered lines
        CHECK_EQ(s.nodeCount,280);
        std::set<int> reached{0},orders;
        std::array<int,mountains::kBranchGenerations> counts{};
        int junctions=0;
        for (std::size_t i=0;i<s.edgeCount;++i) {
            const auto& e=s.edges[i];
            CHECK(e.order<counts.size());
            ++counts.at(e.order);
            CHECK(reached.contains(e.from)); reached.insert(e.to); orders.insert(e.order);
            const auto& a=s.nodes[e.from]; const auto& b=s.nodes[e.to];
            CHECK(b.width<=a.width);
            CHECK(b.height<=a.height);
            CHECK(core::hypot(a.x-b.x,a.y-b.y)>core::kZero);
        }
        CHECK_EQ(reached.size(),std::size_t(s.nodeCount));
        CHECK_EQ(orders.size(),std::size_t(mountains::kBranchGenerations));
        for (std::size_t order=0;order<counts.size();++order) CHECK_EQ(counts[order],9<<order);
        for (std::size_t i=0;i<s.nodeCount;++i) {
            const auto& n=s.nodes[i];
            CHECK(n.height>=core::kZero && n.height<=core::kOne);
            CHECK(n.lift>=core::kZero && n.lift<=n.height*Fixed::ratio(14,100));
            if (n.degree>=3) ++junctions;
            else CHECK_EQ(n.lift,core::kZero);
            if (n.degree==1) CHECK_EQ(n.height,core::kZero);
            else CHECK(n.height>s.nodes[0].height*Fixed::ratio(28,100));
        }
        CHECK_EQ(junctions,91);
    }
}

TEST(mountain_shape_daughters_retain_reach_and_height_at_separated_junctions) {
    constexpr auto segments=mountains::kSegmentsPerBranch;
    CHECK_EQ(segments,3);
    for (std::uint64_t seed : {1,7,42,127}) {
        const auto s=mountains::makeSkeleton(seed);
        // Each branch has two separated anchors followed by a terminal taper.
        const auto length=[&](std::size_t i) {
            const auto& start=s.nodes[s.edges[i].from];
            const auto& tip=s.nodes[s.edges[i+segments-1].to];
            return core::hypot(tip.x-start.x,tip.y-start.y);
        };
        std::array<int,mountains::kMaxNodes> daughters{};
        for (std::size_t i=0;i<s.edgeCount;i+=segments) {
            const auto& start=s.nodes[s.edges[i].from];
            const auto& first=s.nodes[s.edges[i].to];
            const auto& second=s.nodes[s.edges[i+1].to];
            const auto& tip=s.nodes[s.edges[i+2].to];
            CHECK_EQ(s.edges[i].to,s.edges[i+1].from);
            CHECK_EQ(s.edges[i+1].to,s.edges[i+2].from);
            CHECK_EQ(tip.height,core::kZero);
            CHECK(first.height>=start.height*Fixed::ratio(93,100));
            CHECK(first.height<=start.height*Fixed::ratio(98,100));
            CHECK(second.height>=start.height*Fixed::ratio(78,100));
            CHECK(second.height<=start.height*Fixed::ratio(86,100));
            const auto along=[&](const mountains::Node& n) {
                return ((n.x-start.x)*(tip.x-start.x)+(n.y-start.y)*(tip.y-start.y))/length(i)/length(i);
            };
            CHECK(along(first)>Fixed::ratio(30,100) && along(first)<Fixed::ratio(37,100));
            CHECK(along(second)>Fixed::ratio(64,100) && along(second)<Fixed::ratio(70,100));
            if (s.edges[i].order==0) {
                CHECK(length(i)>Fixed::fromInt(2999) && length(i)<Fixed::fromInt(4200));
                continue;
            }
            bool parentFound=false;
            for (std::size_t j=0;j<i;j+=segments) {
                if (s.edges[j].to!=s.edges[i].from && s.edges[j+1].to!=s.edges[i].from) continue;
                parentFound=true;
                ++daughters[s.edges[i].from];
                CHECK_EQ(s.edges[i].order,s.edges[j].order+1);
                const auto ratio=length(i)/length(j);
                CHECK(ratio>Fixed::ratio(59999,100000) && ratio<Fixed::ratio(70001,100000));
            }
            CHECK(parentFound);
        }
        for (std::size_t i=0;i<s.edgeCount;i+=segments) {
            const int expected=s.edges[i].order+1<mountains::kBranchGenerations ? 1 : 0;
            CHECK_EQ(daughters[s.edges[i].to],expected);
            CHECK_EQ(daughters[s.edges[i+1].to],expected);
            CHECK_EQ(daughters[s.edges[i+2].to],0);
        }
    }
}

TEST(mountain_shape_terminal_taper_has_a_smooth_zero_height_ending) {
    mountains::Skeleton s;
    s.nodeCount=2; s.edgeCount=1; s.edges[0]={0,1,4};
    s.nodes[0]={core::kZero,core::kZero,core::kOne,Fixed::fromInt(100),core::kZero,1};
    s.nodes[1]={Fixed::fromInt(1000),core::kZero,core::kZero,Fixed::fromInt(100),core::kZero,1};
    CHECK(mountains::sampleSkeleton(s,Fixed::fromInt(250),core::kZero).height()>Fixed::ratio(8,10));
    CHECK(mountains::sampleSkeleton(s,Fixed::fromInt(999),core::kZero).height()<Fixed::ratio(1,1000000));
    CHECK_EQ(mountains::sampleSkeleton(s,Fixed::fromInt(1001),core::kZero).height(),core::kZero);
}

TEST(mountain_shape_rotated_support_fits_index_for_many_seeds) {
    for (std::uint64_t seed=0;seed<256;++seed) {
        const auto s=mountains::regionSkeleton(seed,static_cast<int>(seed%7)-3,static_cast<int>(seed%11)-5);
        for (std::size_t i=0;i<s.edgeCount;++i) {
            const auto& a=s.nodes[s.edges[i].from]; const auto& b=s.nodes[s.edges[i].to];
            const auto width=std::max(a.width,b.width);
            for (const auto* n : {&a,&b}) {
                CHECK(core::abs(n->x)+width<Fixed::fromInt(mountains::kSupportMetres));
                CHECK(core::abs(n->y)+width<Fixed::fromInt(mountains::kSupportMetres));
                // Removing jitter leaves a rotated local footprint. Include
                // both 100 m placement warps before checking the circular mask.
                CHECK(core::hypot(n->x-s.centreX,n->y-s.centreY)+width+Fixed::fromInt(150)<
                      Fixed::fromInt(mountains::kLocalSupportMetres));
            }
        }
    }
}

TEST(mountain_shape_raises_junctions_and_varies_peak_heights) {
    const auto s=mountains::makeSkeleton(42);
    auto unraised=s;
    for (auto& n:unraised.nodes) n.lift=core::kZero;
    std::set<std::int64_t> heights;
    for (std::size_t i=0;i<s.nodeCount;++i) {
        const auto& n=s.nodes[i]; if (n.degree<3) continue;
        const auto before=mountains::sampleSkeleton(unraised,n.x,n.y);
        const auto after=mountains::sampleSkeleton(s,n.x,n.y);
        CHECK_EQ(before.body,after.body);
        CHECK(after.height()>before.height());
        CHECK(after.height()<=Fixed::ratio(115,100));
        heights.insert((after.height()*1000).roundToInt());
    }
    CHECK(heights.size()>=5);
    // First primary arm must not have one global compass bearing.
    std::set<int> bearings;
    for (int y=-1;y<=1;++y) for (int x=-1;x<=1;++x) {
        const auto region=mountains::regionSkeleton(42,x,y);
        const auto& e=region.edges[0];
        const auto dx=region.nodes[e.to].x-region.nodes[e.from].x;
        const auto dy=region.nodes[e.to].y-region.nodes[e.from].y;
        bearings.insert((dx<core::kZero ? 1:0)+(dy<core::kZero ? 2:0)+(core::abs(dx)>core::abs(dy) ? 4:0));
    }
    CHECK(bearings.size()>=4);
}

TEST(mountain_shape_spatial_cache_matches_full_skeleton_after_eviction) {
    const auto s=mountains::makeSkeleton(42);
    constexpr int extent=mountains::kSupportMetres;
    for (int y=-extent-1;y<=extent+1;y+=701) for (int x=-extent-1;x<=extent+1;x+=617) {
        const auto wx=Fixed::fromInt(x),wy=Fixed::fromInt(y);
        const auto direct=mountains::sampleSkeleton(s,wx,wy);
        const auto cached=mountains::sample(42,wx,wy);
        CHECK_EQ(cached.body,direct.body); CHECK_EQ(cached.junction,direct.junction);
    }
    // Exercise all five 64-bit words and every descendant's foot.
    for (std::size_t i=0;i<s.edgeCount;++i) {
        const auto& a=s.nodes[s.edges[i].from]; const auto& b=s.nodes[s.edges[i].to];
        for (int part : {0,1,2,3,4}) for (int offset : {-1,0,1}) {
            const auto t=Fixed::ratio(part,4);
            const auto wx=core::lerp(a.x,b.x,t)+a.width*offset/2;
            const auto wy=core::lerp(a.y,b.y,t);
            const auto direct=mountains::sampleSkeleton(s,wx,wy),indexed=mountains::sample(42,wx,wy);
            CHECK_EQ(indexed.body,direct.body); CHECK_EQ(indexed.junction,direct.junction);
        }
    }
    const auto x=Fixed::fromInt(-8192),y=Fixed::fromInt(4096);
    const auto before=mountains::worldSample(42,x,y).height();
    for (int i=0;i<40;++i) (void)mountains::worldSample(127+i,Fixed::fromInt(i*8192),y);
    CHECK_EQ(before,mountains::worldSample(42,x,y).height());
    Fixed threaded;
    std::thread worker([&] { threaded=mountains::worldSample(42,x,y).height(); });
    worker.join(); CHECK_EQ(before,threaded);
}

TEST(mountain_shape_world_stencil_matches_unclipped_five_by_five_union) {
    constexpr auto side=mountains::kRegionMetres;
    for (std::uint64_t seed : {1,7,42}) {
        for (int ix : {-8193,-1,0,4096,8191,8192,14336}) for (int iy : {-8192,0,8191}) {
            const auto x=Fixed::ratio(ix*1000+1,1000),y=Fixed::ratio(iy*1000-1,1000);
            const auto rx=mountains::floorDiv(x.raw,Fixed::fromInt(side).raw);
            const auto ry=mountains::floorDiv(y.raw,Fixed::fromInt(side).raw);
            mountains::Shape direct;
            for (auto gy=ry-2;gy<=ry+2;++gy) for (auto gx=rx-2;gx<=rx+2;++gx) {
                const auto s=mountains::regionSkeleton(seed,gx,gy);
                const auto shape=mountains::sampleSkeleton(s,x-Fixed::fromInt(gx*side+side/2),
                                                            y-Fixed::fromInt(gy*side+side/2));
                if (shape.height()>direct.height()) direct=shape;
            }
            const auto indexed=mountains::worldSample(seed,x,y);
            CHECK_EQ(indexed.body,direct.body); CHECK_EQ(indexed.junction,direct.junction);
        }
    }
}

TEST(mountain_shape_has_smooth_feet_and_no_world_axis_seam) {
    CHECK_EQ(mountains::crest(Fixed::fromInt(1000),Fixed::fromInt(1000)),core::kZero);
    CHECK(mountains::crest(Fixed::fromInt(999),Fixed::fromInt(1000))<Fixed::ratio(1,100000));
    const auto continuous=[](int boundary,int p,bool vertical) {
        const auto at=[&](Fixed offset) {
            const auto a=Fixed::fromInt(boundary)+offset,b=Fixed::fromInt(p);
            return mountains::worldSample(42,vertical ? a : b,vertical ? b : a).height();
        };
        const auto centre=at(core::kZero);
        for (int sign : {-1,1}) {
            const auto coarse=core::abs(at(Fixed::ratio(sign,1000))-centre);
            const auto fine=core::abs(at(Fixed::ratio(sign,10000))-centre);
            // Higher outer spurs have steeper legitimate slopes. A real seam
            // would not converge to the boundary value as distance shrinks.
            CHECK(coarse<Fixed::ratio(1,10000));
            CHECK(fine<=coarse/5+Fixed::ratio(1,10000000));
        }
    };
    for (int y=-8000;y<=8000;y+=127) for (bool vertical : {false,true}) continuous(0,y,vertical);
    for (int boundary : {-8192,0,8192}) for (int p=-5000;p<=5000;p+=257) {
        for (bool vertical : {false,true}) continuous(boundary,p,vertical);
    }
}

TEST(mountain_dem_filter_preserves_a_slope_but_reduces_small_peaks) {
    TerrainReference r;
    r.width=r.height=65; r.spacing=Fixed::fromInt(16); r.scale=Fixed::ratio(1,100);
    r.heights.resize(65*65);
    for (int y=0;y<65;++y) for (int x=0;x<65;++x) r.heights[y*65+x]=10000+x*30+y*10;
    for (int i=3;i<=7;++i) {
        const auto u=Fixed::ratio(i,10),v=Fixed::ratio(1,2);
        CHECK(core::abs(r.sampleMountain(u,v)-r.sample(u,v))<Fixed::ratio(1,10000));
    }
    std::fill(r.heights.begin(),r.heights.end(),10000);
    r.heights[32*65+32]=50000;
    const auto centre=Fixed::ratio(1,2),base=r.scale*10000;
    const auto sharp=r.sample(centre,centre)-base;
    const auto filtered=r.sampleMountain(centre,centre)-base;
    CHECK(filtered>sharp*Fixed::ratio(1,5));
    CHECK(filtered<sharp*Fixed::ratio(3,10));
}

TEST(staged_foundation_has_high_preerosion_peaks_without_clipping_or_lifting_plains) {
    auto w=simpleCountry(24); auto p=analyticParams(); p.width=p.height=24; p.erosionPasses=0;
    w.primaryHeightField.assign(w.cells.size(),100);
    w.macroHeightField.resize(w.cells.size());
    for (int y=0;y<w.height;++y) for (int x=0;x<w.width;++x)
        w.macroHeightField[std::size_t(y)*w.width+x]=1000-60*std::abs(x-12);
    // A lower post-thermal maximum used to clip a band of equal-height tops.
    const auto f=buildTerrainFoundation(w,p,0,900);
    const auto x=Fixed::fromInt(12*kMetresPerCell),y=x;
    const auto peak=f->sample(x,y,TerrainStage::Tectonics);
    const auto shoulder=f->sample(x-Fixed::fromInt(kMetresPerCell),y,TerrainStage::Tectonics);
    CHECK(peak>Fixed::fromInt(4200) && peak<Fixed::fromInt(4300));
    CHECK(peak-shoulder>Fixed::fromInt(300));
    CHECK_EQ(f->sample(x,y,TerrainStage::Noise),Fixed::ratio(2295,10));
    CHECK_EQ(f->heightDm[1],f->heightDm[2]); CHECK_EQ(f->heightDm[3],f->heightDm[4]);
    p.erosionPasses=4;
    const auto eroded=buildTerrainFoundation(w,p,0,900);
    CHECK_EQ(eroded->heightDm[1],f->heightDm[1]);
    CHECK(eroded->heightDm[2]!=eroded->heightDm[1]);
    // The thermal stage removes and never adds, and it never lifts a plain -
    // which is what this test is named for and is a stronger statement than the
    // equality it used to make.
    //
    // It used to conserve mass because it was a diffusion, and a diffusion is
    // also why it did nothing: on a uniform over-steep slope every cell
    // receives from above what it gives below. It is a talus cut now, and a cut
    // only removes. Depositing the material at the foot was tried twice and
    // measured to make the ground steeper both times - many faces shed into one
    // hollow, each within its own bound and the sum within nobody's.
    const auto before=std::accumulate(eroded->heightDm[1].begin(),eroded->heightDm[1].end(),
                                      std::int64_t(0));
    const auto after=std::accumulate(eroded->heightDm[2].begin(),eroded->heightDm[2].end(),
                                     std::int64_t(0));
    CHECK(after<before);
    for (std::size_t i=0;i<eroded->heightDm[1].size();++i)
        CHECK(eroded->heightDm[2][i]<=eroded->heightDm[1][i]);
    CHECK(eroded->heightDm[4]!=eroded->heightDm[3]);
    w.terrainFoundation=eroded;
    const auto quant=world::streaming::hsimQuantisationFor(w);
    const auto bounds=world::ringHeightBounds(w,{x,y},{x+Fixed::fromInt(512),y+Fixed::fromInt(512)},4);
    for (const auto& stage:eroded->heightDm) for (auto dm:stage) {
        const auto h=Fixed::ratio(dm,10);
        CHECK(h>=quant.low && h<=quant.high);
        CHECK(core::abs(quant.dequantise(quant.quantise(h))-h)<=quant.resolution());
        CHECK(h.toDouble()>=bounds.first && h.toDouble()<=bounds.second);
    }
}

TEST(staged_foundation_macro_joins_are_smooth_without_overshooting_crests) {
    for (bool vertical:{false,true}) for (bool crest:{false,true}) {
        auto w=simpleCountry(24); auto p=analyticParams(); p.width=p.height=24; p.erosionPasses=0;
        w.primaryHeightField.assign(w.cells.size(),100);
        w.macroHeightField.resize(w.cells.size());
        for (int y=0;y<w.height;++y) for (int x=0;x<w.width;++x) {
            const int t=vertical?y:x;
            w.macroHeightField[std::size_t(y)*w.width+x]=crest?100-3*std::abs(t-16):
                (t<=16?20+3*t:68+9*(t-16));
        }
        const auto f=buildTerrainFoundation(w,p,0,1000);
        // Macro node 16, wherever the cell size puts it.
        constexpr int node=16*kMetresPerCell;
        const auto at=[&](int t) {
            return f->sample(Fixed::fromInt(vertical?node:t),Fixed::fromInt(vertical?t:node),TerrainStage::Tectonics);
        };
        // Macro node 16 happens to lie on H64. Bilinear upsampling leaves a
        // 1.6 m second difference here, even though this is a broad landform.
        const auto centre=at(node),left=at(node-64),right=at(node+64);
        CHECK(core::abs(left+right-centre*2)<Fixed::ratio(7,10));
        CHECK_EQ(centre,Fixed::ratio((crest?100:68)*22950/1000,10));
        if (crest) {
            CHECK(centre>left && centre>right);
            for (auto h:f->heightDm[1]) CHECK(h<=2295 && h>=1193);
        } else {
            for (int t=64;t<=24*kMetresPerCell;t+=64) CHECK(at(t)>=at(t-64));
        }
        CHECK_EQ(f->heightDm[1],f->heightDm[4]); // zero erosion means no hidden shape pass
    }
}

TEST(staged_foundation_resampling_preserves_planes_and_constant_boundaries) {
    auto w=simpleCountry(24); auto p=analyticParams(); p.width=p.height=24; p.erosionPasses=0;
    w.primaryHeightField.assign(w.cells.size(),-20);
    w.macroHeightField.resize(w.cells.size());
    for (int y=0;y<w.height;++y) for (int x=0;x<w.width;++x)
        w.macroHeightField[std::size_t(y)*w.width+x]=40+2*x+y;
    // Exact integer-metre macro heights isolate reconstruction from the first
    // quantisation. Only the final H64 decimetre rounding remains.
    const auto f=buildTerrainFoundation(w,p,0,2295);
    // Clear of the last cell, where the reconstruction holds the edge value
    // rather than the plane: the same margin whatever the cell size.
    const int end=24*kMetresPerCell-960;
    for (int y=640;y<end;y+=192) for (int x=640;x<end;x+=192) {
        const auto actual=f->sample(Fixed::fromInt(x),Fixed::fromInt(y),TerrainStage::Tectonics);
        const auto expected=Fixed::fromInt(40)+Fixed::ratio(2*x+y,kMetresPerCell);
        CHECK(core::abs(actual-expected)<Fixed::ratio(11,100));
    }
    for (auto h:f->heightDm[0]) CHECK_EQ(h,-200);
    CHECK_EQ(f->sample(Fixed::fromInt(-64),Fixed::fromInt(-64),TerrainStage::Tectonics),Fixed::fromInt(40));
    CHECK_EQ(f->sample(Fixed::fromInt(14000),Fixed::fromInt(14000),TerrainStage::Tectonics),Fixed::fromInt(109));
}

TEST(staged_foundation_detail_evidence_ignores_steep_planes_but_keeps_bends) {
    for (bool crest:{false,true}) {
        auto w=simpleCountry(24); auto p=analyticParams(); p.width=p.height=24; p.erosionPasses=0;
        w.primaryHeightField.assign(w.cells.size(),100);
        w.macroHeightField.resize(w.cells.size());
        for (int y=0;y<w.height;++y) for (int x=0;x<w.width;++x)
            w.macroHeightField[std::size_t(y)*w.width+x]=crest?900-60*std::abs(x-10):100+35*x;
        const auto f=buildTerrainFoundation(w,p,0,1000);
        CHECK_EQ(f->needsDetail(5120,5120,5632,5632),crest);
        if (!crest) {
            const auto a=f->sample(Fixed::fromInt(5376),Fixed::fromInt(5376),TerrainStage::Slopes);
            const auto b=f->sample(Fixed::fromInt(5440),Fixed::fromInt(5376),TerrainStage::Slopes);
            CHECK(b-a>Fixed::fromInt(12)); // old slope-only admission requested fine pages
        }
    }
}

TEST(staged_foundation_is_deterministic_and_saves_all_geometry_and_receivers) {
    auto p=analyticParams(); p.width=p.height=24;
    const auto a=generateWorldMap(p),b=generateWorldMap(p);
    CHECK(a.terrainFoundation && b.terrainFoundation); if (!a.terrainFoundation || !b.terrainFoundation) return;
    const auto& f=*a.terrainFoundation;
    CHECK_EQ(f.fingerprint(),b.terrainFoundation->fingerprint());
    const auto payload=saveTerrainFoundation(f);
    const auto copy=loadTerrainFoundation(payload,p.width,p.height);
    CHECK_EQ(copy->fingerprint(),f.fingerprint()); CHECK_EQ(copy->heightDm,f.heightDm);
    std::size_t edges=0;
    for (std::size_t i=0;i<f.receiver.size();++i) if (f.receiver[i]>=0) {
        ++edges; const auto r=std::size_t(f.receiver[i]);
        CHECK(f.heightDm[4][i]>f.heightDm[4][r]); CHECK(f.accumulation[r]>f.accumulation[i]);
    }
    CHECK(edges>0); CHECK(!f.divides.empty());
    for (int mode=0;mode<4;++mode) {
        auto corrupt=json::parse(payload);
        if (mode==0) corrupt["height_dm"][0].erase(corrupt["height_dm"][0].begin());
        if (mode==1) corrupt["receiver"][0]=0;
        if (mode==2) corrupt["height_dm"][0][0]=corrupt["height_dm"][0][0].get<int>()+1;
        if (mode==3) corrupt["version"]=2;
        bool rejected=false;
        try { (void)loadTerrainFoundation(corrupt.dump(),p.width,p.height); } catch (...) { rejected=true; }
        CHECK(rejected);
    }
    auto replay=p; replay.foundationSnapshot=copy; replay.hybridSnapshot=a.hybridTerrain;
    const auto loaded=generateWorldMap(replay);
    CHECK_EQ(loaded.riverDischargeField,a.riverDischargeField);
    p.stagedTerrain=false; CHECK(!generateWorldMap(p).terrainFoundation);
}

TEST(staged_foundation_prepares_only_h64_and_materials_match_refinement) {
    using namespace world::streaming;
    auto p=analyticParams(); p.width=p.height=24;
    const auto w=generateWorldMap(p); const auto graph=buildHydrologyGraph(w);
    PageStore pages(w,graph,hsimQuantisationFor(w));
    pages.prebakeInBackground({2,4});
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    while (pages.prebakeProgress().running && std::chrono::steady_clock::now()<until)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(!pages.prebakeProgress().running);
    CHECK_EQ(pages.prebakeProgress().finestPinned,4);
    CHECK(!pages.resident({10,10,2}));
    TileKey key{10,10,4};
    const auto coarse=pages.page(key); key.level=2; const auto fine=pages.page(key);
    CHECK(coarse && fine); if (!coarse || !fine) return;
    const int a=coarse->base.padding,b=fine->base.padding;
    for (int y=0;y<=8;++y) for (int x=0;x<=8;++x) {
        const auto i=std::size_t(y+a)*coarse->materialWidth+x+a;
        const auto j=std::size_t(y*4+b)*fine->materialWidth+x*4+b;
        for (std::size_t m=0;m<world::kMaterialCount;++m)
            CHECK_EQ(coarse->materials[i*world::kMaterialCount+m],fine->materials[j*world::kMaterialCount+m]);
    }
}

TEST(staged_adaptive_mesh_preserves_both_geometry_endpoints) {
    using namespace world::terrain;
    const auto current=[](double x,double y){return std::array<float,2>{float(x+y),-6000};};
    const auto old=[](double x,double y){return float(x*x/32+y);};
    const auto mesh=makeAdaptiveMesh(16,8,0.01,current,{},0.001,{},old);
    CHECK(mesh->step>=8);
    for (const auto& v:mesh->vertices) {
        CHECK_EQ(v.priorBed,old(v.x*8,v.y*8));
        CHECK_EQ(v.sourceBed,current(v.x*8,v.y*8)[0]);
    }
    CHECK(std::abs(mesh->samplePrior(64,64)-old(64,64))<0.01);
}


TEST(material_is_the_same_ground_at_every_level_of_detail) {
    // A hillside does not change what it is made of because the camera came
    // closer. The weights are a world-space property; the stride is the page's
    // business, and the material pass is handed it only so that it can ignore
    // it. This is the invariant the terrain_lod requirements state and the one
    // a "the rock vanishes when I approach" report is about, so it is worth a
    // test rather than an assurance.
    auto w=simpleCountry(24); auto p=analyticParams(); p.width=p.height=24; p.erosionPasses=3;
    w.primaryHeightField.assign(w.cells.size(),100);
    w.macroHeightField.resize(w.cells.size());
    for (int y=0;y<w.height;++y) for (int x=0;x<w.width;++x)
        w.macroHeightField[std::size_t(y)*w.width+x]=200+900*std::max(0,3-std::abs(x-12))/3;
    for (auto& c : w.cells) { c.sea=false; c.temperature=200; c.moisture=140; }
    w.terrainFoundation=buildTerrainFoundation(w,p,0,1200);
    world::HeightField field(&w,w.seed);

    int looked=0, stony=0;
    for (int cell=6;cell<20;++cell) {
        const auto sx=std::int64_t(cell)*kMetresPerCell/world::kSampleMetres;
        const auto sy=std::int64_t(12)*kMetresPerCell/world::kSampleMetres;
        const auto fine=field.materialsGiven(sx,sy,core::kZero,core::kZero,1);
        ++looked;
        // A fifth, not a quarter: along this flank rock runs 0.1-0.25 at
        // either cell size, and a quarter was cleared by one sample by a
        // thousandth - a check of where the samples fell, not of the ground.
        if (fine.of(world::Material::Rock)>Fixed::ratio(1,5)) ++stony;
        for (const std::int64_t stride : {2,4,16,64}) {
            const auto coarse=field.materialsGiven(sx,sy,core::kZero,core::kZero,stride);
            for (std::size_t m=0;m<world::kMaterialCount;++m)
                CHECK_EQ(fine.weight[m],coarse.weight[m]);
        }
    }
    CHECK(looked>10);
    // And the flank of a nine-hundred-metre ridge - steep, thirty-odd degrees
    // over its kilometre and a half: a gentle flank is grass now, as it ought
    // to be - is stone somewhere along it, or the invariant above is only
    // saying that nothing is rock at any level.
    CHECK(stony>0);
}

TEST(height_and_material_queries_are_cheap_enough_to_build_pages_with) {
    // What a page costs, in the only unit that matters: nanoseconds a query.
    //
    // A frame is not slow because of its triangles when it has forty thousand
    // of them. It is slow because something is asked a million times. These two
    // are asked per vertex and per material texel of every page built, so a
    // hundred nanoseconds here is a millisecond there, and the erosion filter
    // added work to both of them.
    auto w=simpleCountry(24); auto p=analyticParams(); p.width=p.height=24; p.erosionPasses=3;
    w.primaryHeightField.assign(w.cells.size(),100);
    w.macroHeightField.resize(w.cells.size());
    for (int y=0;y<w.height;++y) for (int x=0;x<w.width;++x)
        w.macroHeightField[std::size_t(y)*w.width+x]=200+900*std::max(0,3-std::abs(x-12))/3;
    for (auto& c : w.cells) { c.sea=false; c.temperature=200; c.moisture=140; }
    w.terrainFoundation=buildTerrainFoundation(w,p,0,1200);
    world::HeightField field(&w,w.seed);

    std::printf("  foundation %dx%d at %d m, %zu cells\n", w.terrainFoundation->columns,
                w.terrainFoundation->rows, w.terrainFoundation->step,
                std::size_t(w.terrainFoundation->columns)*w.terrainFoundation->rows);
    constexpr int kQueries=20000;
    const auto timed=[&](const char* what, auto&& call) {
        const auto started=std::chrono::steady_clock::now();
        volatile std::int64_t sink=0;
        for (int i=0;i<kQueries;++i) sink+=call(i);
        const double nanos=std::chrono::duration<double,std::nano>(
                std::chrono::steady_clock::now()-started).count()/kQueries;
        std::printf("  %-22s %7.0f ns a query\n", what, nanos);
        return nanos;
    };
    // Spread over kilometres, which is what building a page looks like.
    const auto height=timed("heightAt spread",[&](int i) {
        const auto x=core::Fixed::fromInt(2000+(i%700)*13),y=core::Fixed::fromInt(2000+(i/700)*13);
        return field.heightAt({x,y}).raw;
    });
    // And clustered inside one carving window. If this is far cheaper the cost
    // is not the field at all - it is whatever the field REBUILDS when a query
    // leaves the last answer's neighbourhood.
    timed("heightAt clustered",[&](int i) {
        const auto x=core::Fixed::fromInt(2000+(i%16)*4),y=core::Fixed::fromInt(2000+(i/16%16)*4);
        return field.heightAt({x,y}).raw;
    });
    timed("foundation.sample",[&](int i) {
        const auto x=core::Fixed::fromInt(2000+(i%700)*13),y=core::Fixed::fromInt(2000+(i/700)*13);
        return w.terrainFoundation->sample(x,y,TerrainStage::Slopes).raw;
    });
    timed("waterLevelAt",[&](int i) {
        const auto x=core::Fixed::fromInt(2000+(i%700)*13),y=core::Fixed::fromInt(2000+(i/700)*13);
        return field.waterLevelAt({x,y}).raw;
    });
    const auto material=timed("materialsGiven",[&](int i) {
        return field.materialsGiven(500+(i%700),500+(i/700),core::kZero,core::kZero,1)
                       .weight[0].raw;
    });
    // Generous bars, so this reports a regression rather than failing on a
    // loaded machine: a page of sixteen thousand vertices has to stay under a
    // frame's worth of work, and these are the two things it spends it on.
    CHECK(height<20000);
    CHECK(material<40000);
}
