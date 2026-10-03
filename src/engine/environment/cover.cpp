#include "engine/environment/cover.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "engine/environment/random.hpp"

namespace engine::environment {

using nlohmann::json;

namespace {

template <class T>
void number(const json& j, const char* key, T& out) {
    if (auto it = j.find(key); it != j.end() && it->is_number()) out = it->get<T>();
}
void pair(const json& j, const char* key, double& lo, double& hi) {
    auto it = j.find(key);
    if (it == j.end()) return;
    if (it->is_number()) lo = hi = it->get<double>();
    else if (it->is_array() && it->size() == 2 && (*it)[0].is_number() && (*it)[1].is_number()) {
        lo = (*it)[0].get<double>();
        hi = (*it)[1].get<double>();
    }
}

} // namespace

CoverRules loadCover(const std::filesystem::path& file, std::vector<CoverProblem>* problems) {
    CoverRules out;
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return out;
    std::ifstream in(file);
    std::stringstream text;
    text << in.rdbuf();
    const auto name = file.filename().string();
    const auto problem = [&](const std::string& what) { if (problems) problems->push_back({name, what}); };
    json j = json::parse(text.str(), nullptr, false, true);
    if (j.is_discarded()) { problem("not valid JSON"); return out; }
    const json* rules = &j;
    if (j.is_object()) {
        if (!j.contains("rules")) { problem("expected {\"rules\": [...]}"); return out; }
        rules = &j["rules"];
    }
    if (!rules->is_array()) { problem("rules must be a list"); return out; }
    for (std::size_t i = 0; i < rules->size(); ++i) {
        const auto& r = (*rules)[i];
        const std::string where = "rules[" + std::to_string(i) + "]";
        if (!r.is_object()) { problem(where + " must be an object"); continue; }
        CoverRule rule;
        if (auto it = r.find("zone"); it != r.end() && it->is_string()) rule.zone = it->get<std::string>();
        else { problem(where + " needs \"zone\" (a zone name or \"*\")"); continue; }
        rule.anyZone = rule.zone == "*";
        if (auto it = r.find("tier"); it != r.end()) {
            const auto t = it->is_string() ? it->get<std::string>() : std::string();
            if (t == "ground") rule.tier = CoverTier::Ground;
            else if (t == "secondary") rule.tier = CoverTier::Secondary;
            else problem(where + ": tier must be ground or secondary");
        }
        if (auto it = r.find("models"); it != r.end()) {
            if (it->is_object()) {
                for (auto m = it->begin(); m != it->end(); ++m)
                    if (m->is_number()) rule.models.push_back({m.key(), m->get<double>()});
            } else if (it->is_array()) {
                for (const auto& m : *it) {
                    if (m.is_string()) rule.models.push_back({m.get<std::string>(), 1});
                    else if (m.is_array() && m.size() == 2 && m[0].is_string() && m[1].is_number())
                        rule.models.push_back({m[0].get<std::string>(), m[1].get<double>()});
                }
            }
        }
        if (auto it = r.find("foliage"); it != r.end() && it->is_string()) rule.foliage = it->get<std::string>();
        if (rule.tier == CoverTier::Secondary && rule.models.empty()) problem(where + ": a secondary rule needs models");
        if (rule.tier == CoverTier::Ground && rule.foliage.empty()) problem(where + ": a ground rule needs foliage");
        number(r, "density", rule.density);
        pair(r, "patch", rule.patchSmall, rule.patchLarge);
        number(r, "patch_contrast", rule.patchContrast);
        number(r, "patch_elongation", rule.patchElongation);
        double cmin = rule.clusterMin, cmax = rule.clusterMax;
        pair(r, "cluster", cmin, cmax);
        rule.clusterMin = int(cmin); rule.clusterMax = int(cmax);
        number(r, "cluster_radius", rule.clusterRadius);
        pair(r, "scale", rule.scaleMin, rule.scaleMax);
        number(r, "height", rule.height);
        if (auto it = r.find("tint"); it != r.end() && it->is_array() && it->size() == 3)
            for (int k = 0; k < 3; ++k) rule.tint[k] = (*it)[k].get<float>();
        if (auto it = r.find("tag"); it != r.end() && it->is_string()) rule.tag = it->get<std::string>();
        if (auto it = r.find("by"); it != r.end()) {
            if (!it->is_array()) problem(where + ": by must be a list of modifiers");
            else for (const auto& m : *it) {
                CoverModifier mod;
                if (m.contains("field")) { mod.source = CoverModifier::Source::Field; mod.name = m["field"].get<std::string>(); }
                else if (m.contains("zone")) { mod.source = CoverModifier::Source::ZoneScalar; mod.name = m["zone"].get<std::string>(); }
                else if (m.contains("mask")) { mod.source = CoverModifier::Source::Mask; mod.name = m["mask"].get<std::string>(); }
                else { problem(where + ": a modifier names a field, a zone scalar or a mask"); continue; }
                double lo = 0, hi = 1;
                pair(m, "range", lo, hi);
                mod.lo = float(lo); mod.hi = float(hi);
                if (auto inv = m.find("invert"); inv != m.end() && inv->is_boolean()) mod.invert = inv->get<bool>();
                number(m, "floor", mod.floor);
                if (mod.source == CoverModifier::Source::Field) {
                    if (auto id = fieldId(mod.name)) mod.index = *id;
                    else { problem(where + ": unknown field \"" + mod.name + "\""); continue; }
                } else if (mod.source == CoverModifier::Source::ZoneScalar) {
                    auto at = std::find_if(std::begin(kZoneScalarNames), std::end(kZoneScalarNames),
                                           [&](const char* n) { return mod.name == n; });
                    if (at == std::end(kZoneScalarNames)) { problem(where + ": unknown zone scalar \"" + mod.name + "\""); continue; }
                    mod.index = std::uint8_t(at - std::begin(kZoneScalarNames));
                }
                rule.modifiers.push_back(mod);
            }
        }
        if (rule.modifiers.size() > 4 && rule.tier == CoverTier::Ground)
            problem(where + ": a ground rule takes at most four modifiers (the GPU table has room for four)");
        out.rules.push_back(std::move(rule));
    }
    return out;
}

float modifierFactor(const CoverModifier& m, const CoverInputs& in) {
    float v = 0;
    switch (m.source) {
        case CoverModifier::Source::Field: if (in.fields) v = in.fields->get(m.index, 0.0f); break;
        case CoverModifier::Source::ZoneScalar: if (in.zone) v = scalarAt(*in.zone, m.index); break;
        case CoverModifier::Source::Mask: if (in.masks && m.index < in.masks->size()) v = (*in.masks)[m.index]; break;
    }
    float f = float(smoothstep(m.lo, m.hi, v));
    if (m.invert) f = 1 - f;
    return std::max(m.floor, f);
}

float ruleDensity(const CoverRule& rule, const CoverInputs& in) {
    float d = float(rule.density);
    for (const auto& m : rule.modifiers) d *= modifierFactor(m, in);
    return d;
}

float coverPatch(std::uint64_t seed, const CoverRule& rule, double x, double y, double contourAngle) {
    // Rotate into the contour's frame and stretch along it: patches lie along
    // the slope, not as round islands.
    const double c = std::cos(contourAngle), s = std::sin(contourAngle);
    const double u = (x * c + y * s) / std::max(1.0, rule.patchElongation);
    const double v = -x * s + y * c;
    const double small = valueNoise(seed ^ 0x51a11ULL, u / rule.patchSmall, v / rule.patchSmall);
    const double large = valueNoise(seed ^ 0x1a26eULL, u / rule.patchLarge, v / rule.patchLarge);
    const double p = large * 0.65 + small * 0.35;
    const double k = std::clamp(rule.patchContrast, 0.0, 1.0);
    // Contrast narrows the soft edge around the middle of the field, turning a
    // gentle mottle into islands with clear ground between them.
    const double width = 0.5 * (1 - k) + 0.05;
    const double f = smoothstep(0.5 - width, 0.5 + width, p);
    return float(1 + (f - 1) * k);
}

std::vector<std::array<float, 4>> coverTable(const CoverRules& rules, std::size_t zoneTypes) {
    std::vector<std::array<float, 4>> table(zoneTypes * kCoverRows, std::array<float, 4>{0, 0, 0, 0});
    for (std::size_t z = 0; z < zoneTypes; ++z) {
        const CoverRule* pick = nullptr;
        for (const auto& r : rules.rules)
            if (r.tier == CoverTier::Ground && !r.anyZone && r.zoneId == z) { pick = &r; break; }
        if (!pick)
            for (const auto& r : rules.rules)
                if (r.tier == CoverTier::Ground && r.anyZone) { pick = &r; break; }
        auto* row = &table[z * kCoverRows];
        if (!pick) {
            row[0] = {1, 1, 6, 50};   // no rule: as the ground draws it today
            continue;
        }
        row[0] = {float(pick->density), float(pick->height), float(pick->patchSmall), float(pick->patchLarge)};
        const std::size_t used = std::min<std::size_t>(4, pick->modifiers.size());
        row[1] = {float(pick->patchContrast), float(pick->patchElongation), 0, float(used)};
        for (std::size_t m = 0; m < used; ++m) {
            const auto& mod = pick->modifiers[m];
            row[2 + m] = {float(int(mod.source) * 64 + mod.index), mod.lo, mod.hi, (mod.invert ? 1.0f : 0.0f) + 2 * mod.floor};
        }
    }
    return table;
}

} // namespace engine::environment
