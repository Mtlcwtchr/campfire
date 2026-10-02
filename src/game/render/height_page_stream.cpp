#include "game/render/height_page_stream.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace game {

PackedHeightPage packHeightPage(std::shared_ptr<const world::streaming::BakedPage> page) {
    PackedHeightPage out;
    out.source = std::move(page);
    const auto& p = *out.source;
    const auto count = p.base.heightQuantized.size();
    const int side = p.base.width + 2 * p.base.padding;
    for (auto& plane : out.fields) plane.resize(count * 4);
    const auto unorm = [](double value) {
        return static_cast<std::uint16_t>(std::lround(std::clamp(value, 0.0, 1.0) * 65535));
    };
    const world::streaming::HsimQuantisation quant{p.base.elevationMin, p.base.elevationMax};
    const auto sea = quant.quantise(core::kZero);
    const bool water = p.water.valid();
    out.mayHaveWater = count == 0;
    auto surface = std::make_shared<world::terrain::SurfacePage>();
    surface->side = side; surface->padding = p.base.padding; surface->step = p.base.sampleMetres;
    surface->version = p.ground;   // the dug ground it shows, nought for the generator's
    surface->bed.resize(count); surface->head.resize(count);
    const double low = p.base.elevationMin.toDouble();
    const double range = (p.base.elevationMax - p.base.elevationMin).toDouble();
    for (std::size_t i = 0; i < count; ++i) {
        // Signed centimetres are biased, not re-quantised into H_sim.
        out.fields[0][i * 4] = static_cast<std::uint16_t>(32768 +
            (i < p.large.deltaQuantized.size() ? p.large.deltaQuantized[i] : 0));
        out.fields[0][i * 4 + 1] = static_cast<std::uint16_t>(32768 +
            (i < p.medium.deltaQuantized.size() ? p.medium.deltaQuantized[i] : 0));
        auto head = water ? p.water.surfaceQuantized[i] : sea;
        const bool ocean = water && p.water.waterBodyId[i] == world::streaming::kOceanWaterBodyId;
        bool lake = water && !ocean && p.water.waterBodyId[i] != world::streaming::kInvalidWaterBodyId;
        // Classification is not coverage. A dry shore texel next to a lake
        // must filter as lake, not as ocean with a head of zero. The padded
        // border makes this stencil identical on neighbouring pages.
        if (water && !ocean && !lake) {
            const auto bankHead = head;
            int nearest = 3;
            std::size_t lakeNeighbour = i;
            const int x=int(i%side),y=int(i/side);
            for (int dy=-1;dy<=1;++dy) for (int dx=-1;dx<=1;++dx) {
                const int nx=x+dx,ny=y+dy,distance=dx*dx+dy*dy;
                if (nx<0 || ny<0 || nx>=side || ny>=side || distance>=nearest) continue;
                const auto n=std::size_t(ny)*side+nx;
                if (p.water.waterBodyId[n]<=world::streaming::kOceanWaterBodyId) continue;
                if (bankHead!=sea && bankHead!=p.water.surfaceQuantized[n]) continue;
                lake=true;nearest=distance;head=p.water.surfaceQuantized[n];
                lakeNeighbour=n;
            }
            // The lake's level lent to a dry bank is for filtering only: it
            // must never stand above the bank's own ground. Where the ground
            // outside a lake falls away below the lake (the far side of its
            // rim, an outlet slope) the borrowed level made water there - a
            // lake hanging in the air over the valley beside it.
            if (head != bankHead) {
                const double bed = low + p.base.heightQuantized[i] * range / 65535.0 +
                    ((i < p.large.deltaQuantized.size() ? p.large.deltaQuantized[i] : 0) +
                     (i < p.medium.deltaQuantized.size() ? p.medium.deltaQuantized[i] : 0)) * 0.01;
                const double level = low + head * range / 65535.0;
                if (level > bed && range > 0) {
                    if (std::getenv("ASR_WATER_DEBUG") && level - bed > 0.5)
                        std::fprintf(stderr, "lake-bank page=%d,%d level=%.2f bank=%.2f (%.2f m under) lake-cell-bed=%.2f\n",
                                     p.base.key.x, p.base.key.y, level, bed, level - bed,
                                     low + p.base.heightQuantized[lakeNeighbour] * range / 65535.0),
                        std::fprintf(stderr, "    bank-base=%.2f bank-deltas=%.2f\n",
                                     low + p.base.heightQuantized[i] * range / 65535.0,
                                     ((i < p.large.deltaQuantized.size() ? p.large.deltaQuantized[i] : 0) +
                                      (i < p.medium.deltaQuantized.size() ? p.medium.deltaQuantized[i] : 0)) * 0.01);
                    head = static_cast<std::uint16_t>(std::clamp(
                        std::floor((bed - low) / range * 65535.0), 0.0, 65535.0));
                }
            }
        }
        out.fields[0][i * 4 + 2] = head;
        out.fields[0][i * 4 + 3] = 0;
        double weights[6]{1, 0, 0, 0, 0, 0};
        if (p.materialWidth && p.materialMetres > 0 && !p.materials.empty()) {
            const double gx = double(i % side) * p.base.sampleMetres / p.materialMetres;
            const double gy = double(i / side) * p.base.sampleMetres / p.materialMetres;
            const int x = std::min(int(gx), int(p.materialWidth) - 1);
            const int y = std::min(int(gy), int(p.materialWidth) - 1);
            const int x1 = std::min(x + 1, int(p.materialWidth) - 1);
            const int y1 = std::min(y + 1, int(p.materialWidth) - 1);
            const double fx = std::clamp(gx - x, 0.0, 1.0), fy = std::clamp(gy - y, 0.0, 1.0);
            double sum = 0;
            for (int m = 0; m < 6; ++m) {
                const auto at = [&](int a, int b) { return p.materials[(b * p.materialWidth + a) * 6 + m] / 255.0; };
                weights[m] = std::lerp(std::lerp(at(x, y), at(x1, y), fx),
                                       std::lerp(at(x, y1), at(x1, y1), fx), fy);
                sum += weights[m];
            }
            if (sum > 0) for (auto& w : weights) w /= sum;
        }
        for (int m = 0; m < 4; ++m) out.fields[1][i * 4 + m] = unorm(weights[m]);
        out.fields[2][i * 4] = unorm(weights[4]);
        out.fields[2][i * 4 + 1] = unorm(weights[5]);
        out.fields[2][i * 4 + 2] = water ? std::uint16_t(p.water.coverage[i]) * 257u : 0;
        out.fields[3][i * 4] = unorm(water ? (p.water.flowX[i] / 127.0 + 1.0) * 0.5 : 0.5);
        out.fields[3][i * 4 + 1] = unorm(water ? (p.water.flowY[i] / 127.0 + 1.0) * 0.5 : 0.5);
        // The river's share, handed over to the sea across its estuary: the
        // sea sheet comes in as this goes out (water_pages.hlsl, wsSeaWeight),
        // and page water fades with it.
        const std::uint32_t estuary =
                water && i < p.water.estuary.size() ? p.water.estuary[i] : 0u;
        out.fields[3][i * 4 + 2] = water && !ocean && !lake &&
            p.water.riverId[i]!=world::streaming::kInvalidRiverId
                ? static_cast<std::uint16_t>((255u - estuary) * 257u) : 0;
        out.fields[3][i * 4 + 3] = lake ? 65535 : 0;
        // All three channels must be zero, not just coverage: close-up inland
        // water can ignore coverage and use hydraulic depth. With zero kind
        // and coverage, WaterPS takes the ocean branch and clips unconditionally,
        // regardless of waves, bed height or weather. Linear filtering and
        // parent morphs preserve zero when every contributing page is dry.
        out.mayHaveWater = out.mayHaveWater || out.fields[2][i * 4 + 2] != 0 ||
            out.fields[3][i * 4 + 2] != 0 || out.fields[3][i * 4 + 3] != 0;
        surface->bed[i] = float(low + p.base.heightQuantized[i] * range / 65535.0 +
            (int(out.fields[0][i*4]) + int(out.fields[0][i*4+1]) - 65536) * 0.01);
        surface->head[i] = float(low + out.fields[0][i*4+2] * range / 65535.0);
    }
    out.surface = std::move(surface);
    return out;
}

HeightPageStream::HeightPageStream(world::streaming::PageStore& pages)
    : pages_(pages), pool_(pages.workerPool()) {
    using Task = world::streaming::TerrainWorkerPool::Task;
    source_ = pool_.add([this](std::size_t) { return work(false); }, Task::Visible);
    try {
        preloadSource_ = pool_.add([this](std::size_t) { return work(true); }, Task::Preload);
    } catch (...) {
        pool_.remove(source_);
        throw;
    }
}
HeightPageStream::~HeightPageStream() {
    { std::lock_guard lock(mutex_); closing_ = true; }
    pool_.remove(source_);
    pool_.remove(preloadSource_);
}
void HeightPageStream::frozen(bool value) {
    {
        std::lock_guard lock(mutex_);
        if (frozen_ == value) return;
        frozen_ = value;
    }
    pool_.notify();
}
void HeightPageStream::wants(const std::vector<Key>& draw, const std::vector<Key>& preload) {
    {
        std::lock_guard lock(mutex_);
        wanted_.clear();
        queue_.clear();
        preloadQueue_.clear();
        std::unordered_set<Key> completed;
        for (const auto& page : done_) completed.insert(page.source->base.key);
        for (const auto* list : {&draw, &preload})
            for (const auto key : *list) {
                if (!wanted_.insert(key).second || inFlight_.contains(key)) continue;
                if (completed.contains(key)) continue;
                (list == &draw ? queue_ : preloadQueue_).push_back(key);
            }
        std::erase_if(done_, [&](const auto& p) { return !wanted_.contains(p.source->base.key); });
        doneBytes_ = 0;
        for (const auto& page : done_) doneBytes_ += uploadBytes(page.source->base.key);
    }
    pool_.notify();
}
std::vector<PackedHeightPage> HeightPageStream::collect() {
    std::vector<PackedHeightPage> result;
    { std::lock_guard lock(mutex_); result.swap(done_); doneBytes_ = 0; }
    if (!result.empty()) pool_.notify(); // only freed completion space can unblock work
    return result;
}
HeightPageStream::Stats HeightPageStream::stats() const {
    std::lock_guard lock(mutex_);
    return {pool_.size(), inFlight_.size(), queue_.size() + preloadQueue_.size(), done_.size(), failed_,
            lastBuildMs_, longestBuildMs_, longestFetchMs_, longestPackMs_, longestPage_};
}
bool HeightPageStream::work(bool preload) {
    Key key;
    {
        std::lock_guard lock(mutex_);
        auto& queue = preload ? preloadQueue_ : queue_;
        if (closing_ || frozen_ || queue.empty() || (preload && !queue_.empty()) ||
            done_.size() + inFlight_.size() >= 256 ||
            doneBytes_ + inFlightBytes_ + uploadBytes(queue.front()) > kCompletionBytes) return false;
        key = queue.front(); queue.pop_front();
        inFlight_.insert(key);
        inFlightBytes_ += uploadBytes(key);
    }
#if ASR_ENABLE_PROFILING
    const auto started = std::chrono::steady_clock::now();
#endif
    auto page = pages_.page(key);
#if ASR_ENABLE_PROFILING
    const auto fetched = std::chrono::steady_clock::now();
#endif
    PackedHeightPage packed;
    if (page && page->base.valid()) packed = packHeightPage(std::move(page));
#if ASR_ENABLE_PROFILING
    const auto finished = std::chrono::steady_clock::now();
#endif
    {
        std::lock_guard lock(mutex_);
#if ASR_ENABLE_PROFILING
        lastBuildMs_ = std::chrono::duration<double, std::milli>(finished - started).count();
        if (lastBuildMs_ >= longestBuildMs_) {
            longestBuildMs_ = lastBuildMs_;
            longestFetchMs_ = std::chrono::duration<double, std::milli>(fetched - started).count();
            longestPackMs_ = std::chrono::duration<double, std::milli>(finished - fetched).count();
            longestPage_ = key;
        }
#endif
        if (!packed.source) ++failed_;
        if (!closing_ && wanted_.contains(key) && packed.source) {
            doneBytes_ += uploadBytes(key);
            done_.push_back(std::move(packed));
        }
        inFlight_.erase(key);
        inFlightBytes_ -= uploadBytes(key);
    }
    return true;
}

} // namespace game

