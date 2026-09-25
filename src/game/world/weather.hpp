#pragma once
#include <array>
#include <cmath>
#include "engine/core/time.hpp"
#include "../../../assets/shaders/weather_model.hlsli"

namespace world::weather {
inline constexpr int kHistory = 17;
inline constexpr std::array<const char*,5> kPresets{"auto","clear","rain","storm","drought"};
struct Sample { WeatherAir air; WeatherSurface surface; };
struct Snapshot {
    // Upload ABI: control, local climate, local readout, then 17 daily forcings.
    // Renderer carries opaque vectors, not gameplay classes.
    std::array<std::array<float,4>,20> data{};
    Sample at(float thermal, float moisture, float height, float x, float y, float drainage) const {
        WeatherSurface left=wxEmpty(), right=wxEmpty();
        WeatherAir previous{}, current{};
        for (int i=0; i<kHistory; ++i) {
            const auto& f=data[3+i];
            const auto a=wxAir(thermal,moisture,height,x,y,f[0],f[1],
                               static_cast<uint>(f[2]),f[3],static_cast<int>(data[0][3]));
            if (i<kHistory-1) left=wxAdvance(left,a,drainage);
            if (i>0) right=wxAdvance(right,a,drainage);
            if (i==kHistory-2) previous=a;
            current=a;
        }
        const float t=data[0][1];
        return {{wxMix(previous.temperature,current.temperature,t),
                 wxMix(previous.precipitation,current.precipitation,t),
                 wxMix(previous.cloud,current.cloud,t),wxMix(previous.wind,current.wind,t)},
                {wxMix(left.snow,right.snow,t),wxMix(left.wet,right.wet,t),wxMix(left.ice,right.ice,t)}};
    }
};
inline float seasonalC(double day, int daysPerSeason, const std::array<float,4>& mids) {
    const double phase=day/std::max(1,daysPerSeason)-0.5;
    const auto whole=static_cast<std::int64_t>(std::floor(phase));
    const auto index=static_cast<std::size_t>((whole%4+4)%4);
    return wxMix(mids[index],mids[(index+1)%4],wxSmooth(0,1,static_cast<float>(phase-std::floor(phase))));
}
inline Snapshot snapshot(std::uint64_t seed, double day, int daysPerSeason=15,
                         const std::array<float,4>& mids={18,32,21,9}, int preset=0) {
    Snapshot s;
    if (!std::isfinite(day)) day=0;
    day=std::clamp(day,0.0,10000000.0);
    const auto whole=static_cast<std::int64_t>(std::floor(day));
    const auto key=static_cast<uint>(seed^(seed>>32));
    s.data[0]={1.0f,static_cast<float>(day-std::floor(day)),
               static_cast<float>(std::fmod(day/std::max(1,daysPerSeason),4.0)),
               static_cast<float>(std::clamp(preset,0,4))};
    for (int i=0;i<kHistory;++i) {
        const auto d=whole-kHistory+2+i;
        // Two-day synoptic episodes; local interpolation removes cell edges.
        const float wet=wxHash(static_cast<int>(std::floor(double(d)/2.0)),0,key);
        s.data[3+i]={seasonalC(double(d),daysPerSeason,mids),0.12f+wet*0.85f,
                     float((key+static_cast<uint>(d)*53u)&65535u),0.35f+wet*1.1f};
    }
    return s;
}
inline double dayAt(std::int64_t tick, const core::TimeConfig& time) {
    return double(tick)/double(std::max<std::int64_t>(1,time.ticksPerDay()));
}
} // namespace world::weather
