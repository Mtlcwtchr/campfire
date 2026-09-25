#pragma once
#include <array>
#include <algorithm>
#include "engine/core/geometry.hpp"
#include "engine/core/time.hpp"
#include "game/world/coords.hpp"

namespace world::weather {
// Fixed equivalent of wxAir.temperature; float rendering is never authority.
inline core::Fixed temperatureAt(std::uint64_t seed, std::int64_t tick,
        const core::TimeConfig& time, const std::array<core::Fixed,4>& mids,
        core::Fixed thermal, core::Fixed height, core::WorldPos p) {
    using core::Fixed;
    const auto smooth=[](Fixed t) {
        t=std::clamp(t,core::kZero,core::kOne);
        return t*t*(Fixed::fromInt(3)-Fixed::fromInt(2)*t);
    };
    const auto hash=[](std::int64_t x,std::int64_t y,std::uint32_t key) {
        std::uint32_t h=std::uint32_t(x)*374761393u+std::uint32_t(y)*668265263u+key*1442695041u;
        h=(h^(h>>13))*1274126177u;
        return Fixed::ratio((h^(h>>16))&65535u,65535);
    };
    const auto ticksPerDay=std::max<std::int64_t>(1,time.ticksPerDay());
    const auto d=floorDiv(tick,ticksPerDay);
    const auto t=Fixed::ratio(floorMod(tick,ticksPerDay),ticksPerDay);
    const auto at=[&](std::int64_t day) {
        const auto phase=Fixed::ratio(day,std::max(1,time.daysPerSeason))-Fixed::ratio(1,2);
        const auto season=floorDiv(phase.raw,core::kOne.raw);
        const auto s=static_cast<std::size_t>(floorMod(season,4));
        const auto f=smooth(Fixed::fromRaw(floorMod(phase.raw,core::kOne.raw)));
        const auto seasonal=core::lerp(mids[s],mids[(s+1)%4],f);
        const auto key=(std::uint32_t(seed^(seed>>32))+std::uint32_t(day)*53u)&65535u;
        const auto x=p.x/Fixed::fromInt(12000), y=p.y/Fixed::fromInt(12000);
        const auto ix=floorDiv(x.raw,core::kOne.raw), iy=floorDiv(y.raw,core::kOne.raw);
        const auto u=smooth(Fixed::fromRaw(floorMod(x.raw,core::kOne.raw)));
        const auto v=smooth(Fixed::fromRaw(floorMod(y.raw,core::kOne.raw)));
        const auto regional=core::lerp(core::lerp(hash(ix,iy,key),hash(ix+1,iy,key),u),
            core::lerp(hash(ix,iy+1,key),hash(ix+1,iy+1,key),u),v);
        const auto continental=Fixed::ratio(135,100)-smooth((thermal-Fixed::ratio(55,100))/Fixed::ratio(40,100));
        return Fixed::fromInt(18)+(thermal-Fixed::ratio(65,100))*Fixed::fromInt(60)+
            (seasonal-Fixed::fromInt(18))*continental-height*Fixed::ratio(4,1000)+
            (regional-Fixed::ratio(1,2))*Fixed::fromInt(7);
    };
    return core::lerp(at(d),at(d+1),t);
}
}
