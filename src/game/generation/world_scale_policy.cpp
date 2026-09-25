#include "game/generation/world_scale_policy.hpp"
#include "engine/core/rng.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace generation {
namespace {
using Json = nlohmann::json;
bool validStep(std::uint32_t step) {
    const auto& steps=WorldScalePolicy::kCoarseSteps;
    return std::find(steps.begin(),steps.end(),step)!=steps.end();
}
std::uint64_t samples(std::int64_t extent,std::uint32_t step) {
    return (std::uint64_t(extent)+step-1)/step+1;
}
std::uint64_t unsignedField(const Json& j,const char* key,std::uint64_t maximum) {
    const auto& v=j.at(key);
    if ((!v.is_number_unsigned() && (!v.is_number_integer() || v.get<std::int64_t>()<0)) ||
        v.get<std::uint64_t>()>maximum) throw std::invalid_argument("invalid world descriptor integer");
    return v.get<std::uint64_t>();
}
}
std::array<double,2> ScaleRule::metres(const WorldDomain& domain) const {
    if (!std::isfinite(value) || value<0 || value>WorldDomain::kMaxExtent)
        throw std::invalid_argument("invalid scale rule value");
    switch (kind) {
    case ScaleClass::Absolute: return {value,value};
    case ScaleClass::WorldRelative:
        if (value>1) throw std::invalid_argument("world-relative value must be a fraction");
        return {value*domain.widthMetres(),value*domain.heightMetres()};
    case ScaleClass::SemiRelative: {
        if (!std::isfinite(exponent) || exponent<0 || exponent>1 ||
            !std::isfinite(minimumMetres) || !std::isfinite(maximumMetres) ||
            minimumMetres<=0 || maximumMetres<minimumMetres || maximumMetres>WorldDomain::kMaxExtent)
            throw std::invalid_argument("invalid semi-relative growth bounds");
        const double scale=std::sqrt(double(domain.areaSquareMetres()))/100000;
        const double length=std::clamp(value*std::pow(scale,exponent),minimumMetres,maximumMetres);
        return {length,length};
    }
    }
    throw std::invalid_argument("unknown scale class");
}
void WorldDescriptor::validate() const {
    if (!validStep(coarseStepMetres) || overviewSamples[0]<2 || overviewSamples[1]<2 ||
        overviewSamples[0]>1024 || overviewSamples[1]>1024)
        throw std::invalid_argument("invalid coarse step or bounded overview dimensions");
}
std::uint64_t WorldDescriptor::coarseSampleCount() const {
    validate();
    return samples(domain.widthMetres(),coarseStepMetres)*samples(domain.heightMetres(),coarseStepMetres);
}
std::uint64_t WorldDescriptor::fingerprint() const {
    validate();
    auto hash=core::splitmix64(0x7363616c65ULL);
    const std::uint64_t fields[]{kFormatVersion,kGeneratorVersion,kPolicyVersion,seed,
        std::uint64_t(domain.widthMetres()),std::uint64_t(domain.heightMetres()),coarseStepMetres,
        overviewSamples[0],overviewSamples[1]};
    for (const auto value:fields) hash=core::splitmix64(hash^core::splitmix64(value));
    return hash;
}
std::string WorldDescriptor::serialize() const {
    validate();
    return Json{{"format_version",kFormatVersion},{"generator","scale-aware"},
        {"generator_version",kGeneratorVersion},{"scale_policy_version",kPolicyVersion},
        {"seed",seed},{"width_m",domain.widthMetres()},{"height_m",domain.heightMetres()},
        {"coarse_step_m",coarseStepMetres},{"overview_columns",overviewSamples[0]},
        {"overview_rows",overviewSamples[1]}}.dump(2);
}
WorldDescriptor WorldDescriptor::parse(std::string_view text) {
    if (text.size()>8192) throw std::invalid_argument("world descriptor is too large");
    const auto j=Json::parse(text);
    if (!j.is_object() || j.at("generator")!="scale-aware" ||
        unsignedField(j,"format_version",kFormatVersion)!=kFormatVersion ||
        unsignedField(j,"generator_version",kGeneratorVersion)!=kGeneratorVersion ||
        unsignedField(j,"scale_policy_version",kPolicyVersion)!=kPolicyVersion)
        throw std::invalid_argument("unsupported world descriptor/generator/policy version");
    WorldDescriptor result;
    result.domain=WorldDomain(std::int64_t(unsignedField(j,"width_m",WorldDomain::kMaxExtent)),
        std::int64_t(unsignedField(j,"height_m",WorldDomain::kMaxExtent)));
    result.seed=unsignedField(j,"seed",UINT64_MAX);
    result.coarseStepMetres=std::uint32_t(unsignedField(j,"coarse_step_m",2048));
    result.overviewSamples={std::uint32_t(unsignedField(j,"overview_columns",1024)),
        std::uint32_t(unsignedField(j,"overview_rows",1024))};
    result.validate();
    return result;
}
WorldDescriptor WorldScalePolicy::describe(const WorldDomain& domain,std::uint64_t seed) const {
    if ((coarseStepMetres && !validStep(coarseStepMetres)) || overviewSideLimit<2 ||
        overviewSideLimit>1024 || coarseSampleBudget<4)
        throw std::invalid_argument("invalid world scale policy");
    WorldDescriptor result;
    result.domain=domain;result.seed=seed;
    result.coarseStepMetres=coarseStepMetres;
    if (!result.coarseStepMetres) {
        result.coarseStepMetres=kCoarseSteps.back();
        for (const auto step:kCoarseSteps)
            if (samples(domain.widthMetres(),step)*samples(domain.heightMetres(),step)<=coarseSampleBudget) {
                result.coarseStepMetres=step;break;
            }
    }
    // Budget a normalized overview independently of physical source spacing.
    // For large worlds even H2048 must be paged; never allocate the estimate here.
    const auto longest=std::max(domain.widthMetres(),domain.heightMetres());
    const auto intervals=std::min<std::uint64_t>(overviewSideLimit-1,samples(longest,result.coarseStepMetres)-1);
    const auto axis=[&](std::int64_t extent) {
        return std::uint32_t((std::uint64_t(extent)*intervals+longest-1)/longest+1);
    };
    result.overviewSamples={axis(domain.widthMetres()),axis(domain.heightMetres())};
    result.validate();
    return result;
}
} // namespace generation

