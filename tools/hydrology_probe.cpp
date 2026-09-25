#include "game/generation/world_map_gen.hpp"
#include "game/world/height_field.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    using core::Fixed;
    generation::WorldMapParams params;
    params.width = params.height = 64;
    params.seed = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 5;
    const auto map = generation::generateWorldMap(params);
    world::HeightField field(&map, params.seed);
    double worst = 0;
    core::WorldPos worstAt{}, dryAt{};
    core::TilePos owner{};
    double gradeMax = 0;
    core::WorldPos gradeAt{}, gradeBefore{};
    std::vector<double> grades;
    for (int y = 2; y < 62; ++y) for (int x = 2; x < 62; ++x) {
        const auto c = field.macro().channelOf({x,y});
        if (!c || !c->wet) continue;
        const auto p = world::MacroWorld::pointOn(*c, Fixed::ratio(1,2));
        const auto water = field.macro().carve(p,core::kZero,core::kZero);
        const double fx=water.flowX.toDouble(), fy=water.flowY.toDouble();
        const double length=std::hypot(fx,fy);
        if (length<0.01) continue;
        double worstGrade=0, previous=0;
        core::WorldPos before{};
        for (int d=-300; d<=300; d+=4) {
            const core::WorldPos at{p.x+Fixed::ratio(std::lround(-fy/length*d*100),100),
                                    p.y+Fixed::ratio(std::lround(fx/length*d*100),100)};
            const double h=field.heightAt(at).toDouble();
            if (d>-300 && std::abs(d)>c->halfWidth.toDouble()+4) {
                const double grade=std::abs(h-previous)/4;
                worstGrade=std::max(worstGrade,grade);
                if (grade>gradeMax) { gradeMax=grade; gradeAt=at; gradeBefore=before; }
            }
            previous=h; before=at;
        }
        grades.push_back(worstGrade);
    }
    std::sort(grades.begin(),grades.end());
    if (!grades.empty()) std::printf("bank grades: median=%.3f p90=%.3f max=%.3f at %.3f,%.3f\n",
        grades[grades.size()/2],grades[grades.size()*9/10],gradeMax,gradeAt.x.toDouble(),gradeAt.y.toDouble());
    if (argc>2) {
        worstAt=gradeBefore; dryAt=gradeAt;
    }
    for (int y = 2; y < 62; ++y) for (int x = 2; x < 62; ++x) {
        if (!map.at({x,y}).river) continue;
        const auto c = field.macro().channelOf({x,y});
        if (!c || !c->wet) continue;
        for (int i = 1; i < 8; ++i) {
            const auto p = world::MacroWorld::pointOn(*c, Fixed::ratio(i,8));
            const auto q = world::MacroWorld::pointOn(*c, Fixed::ratio(i,8) + Fixed::ratio(1,200));
            const double dx = (q.x-p.x).toDouble(), dy = (q.y-p.y).toDouble();
            const double length = std::hypot(dx,dy);
            if (length < 0.01) continue;
            for (int sign : {-1,1}) {
                core::WorldPos previous = p;
                double lastDepth = -1;
                for (double d = 0; d <= c->halfWidth.toDouble()*2+40; d += 4) {
                    const core::WorldPos at{p.x + Fixed::fromDoubleForContent(-dy/length*d*sign),
                                            p.y + Fixed::fromDoubleForContent(dx/length*d*sign)};
                    if (!field.underWater(at)) {
                        if (lastDepth > worst) {
                            worst=lastDepth; owner={x,y};
                            if (argc<=2) { worstAt=previous; dryAt=at; }
                        }
                        break;
                    }
                    lastDepth = (field.waterLevelAt(at)-field.heightAt(at)).toDouble();
                    previous = at;
                }
            }
        }
    }
    std::printf("seed=%llu worstShoreDepth=%.3f owner=%d,%d\n",
        static_cast<unsigned long long>(params.seed), worst, owner.x, owner.y);
    for (int i=-2;i<=3;++i) {
        const auto t = Fixed::fromInt(i);
        const core::WorldPos p{worstAt.x+(dryAt.x-worstAt.x)*t,worstAt.y+(dryAt.y-worstAt.y)*t};
        const auto g = field.piecesAt(p.x,p.y);
        const auto w = field.macro().carve(p,g.country,g.moved);
        const auto over = field.waterOver(p,Fixed::fromInt(4),field.slopeAt(p));
        std::printf("%.3f %.3f bed=%.3f floor=%.3f head=%.3f kind=%d cover=%.3f bank=%.3f country=%.3f detail=%.3f lake=%.3f fill=%.3f stage=%.3f\n",
            p.x.toDouble(),p.y.toDouble(),field.heightAt(p).toDouble(),w.floor.toDouble(),
            field.waterLevelAt(p).toDouble(),int(over.kind),over.cover.toDouble(),w.bankDistance.toDouble(),
            g.country.toDouble(),g.moved.toDouble(),g.lakeLevel.toDouble(),g.lakeDeep.toDouble(),w.surface.toDouble());
    }
}

