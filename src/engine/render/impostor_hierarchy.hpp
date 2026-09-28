#pragma once
// A persistent spatial hierarchy of recursively baked impostors.
//
// Level 0 is a 32 m cell; every level above covers 4x4 of the one below
// (32 -> 128 -> 512 -> 2048 m). Nothing here knows about forests, scatter or
// SDL: the game says what a node contains, the engine decides which nodes
// replace which, keeps baked results in a bounded store and never lets a node
// and any of its descendants be selected in the same cut.
#include "engine/render/impostor_cache.hpp"
#include "engine/render/representation_selector.hpp"
#include <functional>
#include <map>
#include <unordered_map>

namespace engine::render {
inline constexpr int kHierarchyLevels=4;
inline constexpr std::int64_t kHierarchyBase=32;
constexpr std::int64_t hierarchyCell(int level) { return kHierarchyBase<<(2*level); }
ImpostorKey hierarchyKey(int level,double x,double y);
ImpostorKey hierarchyParent(ImpostorKey key);
std::array<ImpostorKey,16> hierarchyChildren(ImpostorKey key); // key.level must be >0
bool hierarchyContains(ImpostorKey ancestor,ImpostorKey key);

// Alpha-weighted box resample to a smaller power-of-two resolution. Depth keeps
// the nearest covered texel (never an average of two surfaces); mips rebuilt.
std::shared_ptr<const ImpostorAtlas> resampleImpostor(const ImpostorAtlas& atlas,unsigned resolution);

// Measures what the runtime view selection actually costs this atlas: from
// directions between the baked ones, the silhouette reprojected from each
// contributing view is compared with the silhouette of the whole surface the
// atlas knows (all 21 views together). Returns metres in the view plane that
// 99% of covered texels are within. Negative for an invalid atlas.
double measureImpostorViewError(const ImpostorAtlas& atlas);
// The same comparison against what the finer level really shows: each child
// reprojected from the views it would itself select (its own yaw and scale).
// Covers texel loss, merging and view selection of the parent in one number.
double measureImpostorAgainstChildren(const ImpostorAtlas& parent,std::span<const ImpostorPlacement> children,
                                      const std::atomic_bool* cancel=nullptr);
// A copy of `parent` whose error is monotone over the hierarchy (Nanite-style):
// error = max over children of their total error, viewError = the measured
// parent-vs-children disagreement. Total = error + viewError >= every child's.
std::shared_ptr<const ImpostorAtlas> withMeasuredError(const std::shared_ptr<const ImpostorAtlas>& parent,
        std::span<const ImpostorPlacement> children,const std::atomic_bool* cancel=nullptr);
inline double impostorTotalError(const ImpostorAtlas& atlas) { return atlas.error+std::max(0.0,atlas.viewError); }

// Bounded CPU store of baked nodes, least recently used first out. An entry may
// be "known empty" (null atlas): a node with nothing to draw is also an answer.
class ImpostorStore {
public:
    struct Entry {
        std::uint64_t revision=0;
        std::shared_ptr<const ImpostorAtlas> atlas;
        double used=0;
    };
    explicit ImpostorStore(std::size_t byteBudget=64*1024*1024,std::size_t maxEntries=65536);
    const Entry* find(ImpostorKey key,std::uint64_t revision) const;
    void touch(ImpostorKey key,double now);
    // False when the atlas alone exceeds the budget; never evicts `key` itself.
    bool put(ImpostorKey key,std::uint64_t revision,std::shared_ptr<const ImpostorAtlas> atlas,double now);
    void erase(ImpostorKey key);
    void clear();
    std::size_t bytes() const { return bytes_; }
    std::size_t size() const { return entries_.size(); }
    std::size_t budget() const { return budget_; }
private:
    static std::size_t cost(const Entry& entry);
    void evict(ImpostorKey keep);
    std::unordered_map<ImpostorKey,Entry,ImpostorKeyHash> entries_;
    std::size_t budget_,maxEntries_,bytes_=0;
};

// What the caller knows about one node this frame.
struct HierarchyNodeInfo {
    bool visible=true;      // inside the frustum and the draw distance
    bool empty=false;       // known to contain nothing representable
    bool mustRefine=false;  // cannot stand for its area (e.g. terrain density owns part)
    bool resident=false;    // complete on the GPU, drawable now
    RepresentationBounds bounds;
    double errorMetres=0;   // baked error, or a lower-bound estimate before baking
    // errorMetres already includes the view-selection error (measured at bake
    // time); otherwise the analytic bound 2r·sin(Δ/2) is added.
    bool viewErrorIncluded=false;
};
struct HierarchyCut {
    std::vector<ImpostorKey> draws;   // never nested
    std::vector<std::pair<ImpostorKey,double>> wanted; // acceptable but not resident, with priority
    std::size_t visited=0;
    // Why nodes were not drawn: outside, below the baked hemisphere, too
    // coarse for the allowance, and too coarse with no finer level possible.
    std::size_t invisible=0,below=0,coarse=0,pruned=0;
    bool budgetLimited=false;
};
struct HierarchyOptions {
    int minLevel=1;                 // levels below are owned by the caller's finer path
    double allowanceScale=1;        // >1 only under budget pressure
    std::size_t maxVisits=200000;
    std::size_t maxDraws=std::numeric_limits<std::size_t>::max();
};
// Coarse-to-fine: a visible node whose error is within the impostor allowance
// is drawn if resident; otherwise it is wanted and its children are only
// descended if one of them may already be drawable (resident below it) or the
// node itself is too coarse. Nodes below minLevel are never selected.
HierarchyCut selectImpostorHierarchy(const camera::ViewState& view,std::span<const ImpostorKey> roots,
    const std::function<HierarchyNodeInfo(ImpostorKey)>& info,std::span<const ImpostorKey> resident,
    std::span<const ImpostorKey> previous,const HierarchyOptions& options={});
} // namespace engine::render

