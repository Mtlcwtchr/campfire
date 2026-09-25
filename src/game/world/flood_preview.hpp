#pragma once
// Bounded connected-water scenario, NOT a probability or a fluid simulation.
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <vector>
#include "game/world/height_field.hpp"
namespace world {
class FloodPreview {
public:
    static constexpr int kSide = 65;
    bool build(const HeightField& field, core::WorldPos centre, double visibleSpan,
               const std::function<bool()>& cancelled = {}) {
        if (!std::isfinite(visibleSpan) || visibleSpan <= 0) { clear(); return false; }
        step_ = std::clamp(std::ceil(visibleSpan / 48.0 / 8.0) * 8.0, 8.0, 64.0);
        left_ = std::floor(centre.x.toDouble()/step_) * step_ - 32 * step_;
        top_ = std::floor(centre.y.toDouble()/step_) * step_ - 32 * step_;
        std::vector<float> height(kSide*kSide), source(kSide*kSide, -1e20f);
        threshold_.assign(kSide*kSide, 9.0f);
        for (int y=0; y<kSide; ++y) for (int x=0; x<kSide; ++x) {
            const int i=y*kSide+x;
            if (i % 64 == 0 && cancelled && cancelled()) { clear(); return false; }
            const core::WorldPos p{core::Fixed::fromDoubleForContent(left_+x*step_), core::Fixed::fromDoubleForContent(top_+y*step_)};
            height[i]=static_cast<float>(field.heightAt(p).toDouble());
            const float level=static_cast<float>(field.waterLevelAt(p).toDouble());
            if (height[i] <= level) { source[i]=level; threshold_[i]=0; }
        }
        // The head reaching a cell is the highest CONNECTED source head. Four
        // neighbours prevent diagonal corner leaks through a bank.
        for (float rise : {1.0f, 3.0f, 6.0f}) {
            const auto reached = connectedHeads(height, source, kSide, rise, cancelled);
            if (reached.empty()) { clear(); return false; }
            for (std::size_t i=0; i<height.size(); ++i)
                if (reached[i] > -1e19f) threshold_[i]=std::min(threshold_[i], rise);
        }
        ++revision_;
        return true;
    }
    static std::vector<float> connectedHeads(const std::vector<float>& height,
            const std::vector<float>& source, int side, float rise,
            const std::function<bool()>& cancelled = {}) {
        if (side <= 0 || !std::isfinite(rise) || rise < 0 ||
            height.size()!=static_cast<std::size_t>(side)*side || source.size()!=height.size()) return {};
        std::vector<float> reached(height.size(), -1e20f);
        std::priority_queue<std::pair<float,int>> todo;
        for (std::size_t i=0; i<height.size(); ++i) if (source[i]>-1e19f) {
            reached[i]=source[i]+rise; todo.push({reached[i],static_cast<int>(i)});
        }
        std::size_t visited = 0;
        while (!todo.empty()) {
            if (visited++ % 128 == 0 && cancelled && cancelled()) return {};
            const auto [head,i]=todo.top(); todo.pop();
            if (head<reached[i]) continue;
            const int x=i%side, y=i/side;
            const int neighbours[]{x>0?i-1:-1,x+1<side?i+1:-1,y>0?i-side:-1,y+1<side?i+side:-1};
            for (int j:neighbours) if (j>=0 && height[j]<=head && reached[j]<head) {
                reached[j]=head; todo.push({head,j});
            }
        }
        return reached;
    }
    // Display interpolation only: NEVER use this value to permit flooding or
    // construction. Connectivity is discrete at the solved grid resolution.
    float at(core::WorldPos p) const {
        if (threshold_.empty()) return -1;
        const double x=(p.x.toDouble()-left_)/step_, y=(p.y.toDouble()-top_)/step_;
        // A margin is unknown: sources outside this local window weren't solved.
        if (x<1 || y<1 || x>=kSide-2 || y>=kSide-2) return -1;
        const int ix=static_cast<int>(x), iy=static_cast<int>(y);
        const float u=static_cast<float>(x-ix),v=static_cast<float>(y-iy);
        const auto sample=[&](int ax,int ay){return threshold_[ay*kSide+ax];};
        return std::lerp(std::lerp(sample(ix,iy),sample(ix+1,iy),u), std::lerp(sample(ix,iy+1),sample(ix+1,iy+1),u),v);
    }
    std::uint64_t revision() const { return revision_; }
    std::array<float, 4> bounds() const {
        if (threshold_.empty()) return {};
        return {static_cast<float>(left_+step_), static_cast<float>(top_+step_),
                static_cast<float>(left_+(kSide-2)*step_), static_cast<float>(top_+(kSide-2)*step_)};
    }
    void clear() { threshold_.clear(); ++revision_; }
private:
    double left_=0,top_=0,step_=8;
    std::vector<float> threshold_;
    std::uint64_t revision_=0;
};
} // namespace world

