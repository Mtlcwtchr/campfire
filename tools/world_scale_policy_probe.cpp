#include "game/generation/world_scale_policy.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

int main(int argc,char** argv) {
    try {
        if (argc!=1 && (argc!=3 || std::string_view(argv[1])!="--output"))
            throw std::invalid_argument("usage: world_scale_policy_probe [--output report.json]");
        using namespace generation;
        using Json=nlohmann::json;
        WorldScalePolicy policy;
        Json cases=Json::array();
        const auto add=[&](std::string_view label,WorldDomain domain,std::uint64_t seed) {
            const auto descriptor=policy.describe(domain,seed);
            Json entry=Json::parse(descriptor.serialize());
            entry["name"]=label;entry["fingerprint"]=descriptor.fingerprint();
            entry["coarse_sample_count_estimate"]=descriptor.coarseSampleCount();
            entry["coarse_int32_height_bytes_estimate"]=descriptor.coarseSampleCount()*4;
            entry["requires_paged_coarse"]=descriptor.coarseSampleCount()>policy.coarseSampleBudget;
            entry["overview_sample_capacity"]=std::uint64_t(descriptor.overviewSamples[0])*descriptor.overviewSamples[1];
            entry["absolute_8m_rule"]=ScaleRule{ScaleClass::Absolute,8}.metres(domain);
            entry["world_relative_quarter"]=ScaleRule{ScaleClass::WorldRelative,0.25}.metres(domain);
            entry["semi_relative_30km_rule"]=ScaleRule{ScaleClass::SemiRelative,30000}.metres(domain);
            if (WorldDescriptor::parse(descriptor.serialize())!=descriptor)
                throw std::runtime_error("descriptor roundtrip failed");
            cases.push_back(std::move(entry));
        };
        for (const auto seed:{1u,7u,11u,42u}) {
            for (const auto side:{100000,250000,1000000,10000000})
                add("scale matrix",WorldDomain(side,side),seed);
            add("rectangular",WorldDomain(10000000,250000),seed);
        }
        for (const auto& preset:kScaleWorldSizes) add(preset.name,scaleWorldPreset(preset.name),42);
        const Json report{{"scope","P1a descriptor/policy only; terrain generation and runtime integration pending"},
            {"terrain_generated",false},{"runtime_presets_changed",false},{"height_samples_allocated",0},
            {"overview_side_limit",policy.overviewSideLimit},{"coarse_sample_budget",policy.coarseSampleBudget},
            {"cases",std::move(cases)}};
        if (argc==3) {
            std::ofstream output(argv[2]);output<<report.dump(2)<<'\n';
            if (!output) throw std::runtime_error("cannot write policy report");
        } else std::cout<<report.dump(2)<<'\n';
        return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}

